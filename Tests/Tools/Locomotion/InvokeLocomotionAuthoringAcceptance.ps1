#requires -Version 7.0
<#
.SYNOPSIS
  Exercises installed locomotion authoring through the real editor's loopback MCP bridge.
.DESCRIPTION
  This is an application driver, not a test-executable substitute. Supply the
  disposable workload project and installed tree explicitly. Mutations use the
  editor's ordinary protected automation transactions; this verifies the same
  transaction/undo boundary used by Inspector multi-edit but does not simulate
  mouse interaction with the Inspector window.
.EXAMPLE
  ./InvokeLocomotionAuthoringAcceptance.ps1 -InstallRoot out/build/RelWithDebInfo/Build `
    -ProjectPath out/locomotion-authoring/project -OutputDirectory out/evidence/locomotion/060/authoring -Port 43179
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$ProjectPath,
	[Parameter(Mandatory)][string]$OutputDirectory,
	[ValidateRange(1024,65535)][int]$Port = 43179,
	[ValidateRange(1,30)][int]$StartupTimeoutMinutes = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallRoot)
$project = [IO.Path]::GetFullPath($ProjectPath)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$editor = Join-Path $install 'Editor/TerraEngine-Editor.exe'
$smokeScene = Join-Path $project 'Assets/Scenes/Smoke.tescene'
$workloadMetadataPath = Join-Path $project 'Assets/Locomotion/workload-metadata.json'
$fixtureManifestPath = Join-Path $install 'Tests/Fixtures/Locomotion/Generated/fixture-assets.json'
foreach($required in @($editor, (Join-Path $project 'game.json'), $smokeScene, $fixtureManifestPath, $workloadMetadataPath)) {
	if(-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required installed-editor input is unavailable: $required" }
}
$workloadMetadata = Get-Content -LiteralPath $workloadMetadataPath -Raw | ConvertFrom-Json
if($workloadMetadata.fixtureId -ne 'locomotion-060-v1' -or $workloadMetadata.seed -ne 60060) {
	throw 'Authoring acceptance may only modify a generated locomotion disposable project.'
}
$fixtureManifest = Get-Content -LiteralPath $fixtureManifestPath -Raw | ConvertFrom-Json
$controllerAsset = @($fixtureManifest.assets | Where-Object { $_.role -eq 'controller' })
if($controllerAsset.Count -ne 1) { throw 'Installed generated fixture manifest must pin exactly one locomotion controller asset.' }
$controllerAssetId = [string]$controllerAsset[0].assetId
$installPrefix = $install.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if($output.Equals($install,[StringComparison]::OrdinalIgnoreCase) -or
	$output.StartsWith($installPrefix,[StringComparison]::OrdinalIgnoreCase)) {
	throw 'Authoring evidence must be written outside the install tree.'
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$run = Join-Path $output ('authoring-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$rpcPath = Join-Path $run 'mcp.jsonl'
$summaryPath = Join-Path $run 'authoring-acceptance.json'
$preferencesPath = Join-Path $run 'editor-preferences.json'
$stdoutPath = Join-Path $run 'editor.stdout.log'
$stderrPath = Join-Path $run 'editor.stderr.log'
$editorDirectory = Join-Path $install 'Editor'
$projectRelative = [IO.Path]::GetRelativePath($project, $smokeScene).Replace('\','/')
$script:rpcId = 0
$script:process = $null
$script:stdoutTask = $null
$script:stderrTask = $null
$script:stdoutPath = $stdoutPath
$script:stderrPath = $stderrPath
$script:rpcPath = $rpcPath
$script:port = $Port
$script:cases = [Collections.Generic.List[object]]::new()
$script:failures = [Collections.Generic.List[string]]::new()
$script:unavailable = [Collections.Generic.List[string]]::new()
$script:latencyClock = [Diagnostics.Stopwatch]::StartNew()
$script:setupStartMs = $null
$script:setupFirstMotionMs = $null

function Write-JsonFile([string]$Path, $Value) {
	$text = ($Value | ConvertTo-Json -Depth 40).Replace("`r`n", "`n").TrimEnd("`n") + "`n"
	[IO.File]::WriteAllText($Path, $text, [Text.UTF8Encoding]::new($false))
}

function Add-Case([string]$Id, [string]$Status, $Evidence, [string]$Detail = '') {
	$script:cases.Add([ordered]@{ id = $Id; status = $Status; detail = $Detail; evidence = $Evidence })
	if($Status -eq 'failed') { $script:failures.Add("$Id`: $Detail") }
	if($Status -eq 'unavailable') { $script:unavailable.Add("$Id`: $Detail") }
}

function Test-FreeLoopbackPort([int]$Candidate) {
	$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Candidate)
	try { $listener.Start(); return $true } catch { return $false } finally { $listener.Stop() }
}

function Invoke-Rpc([string]$Method, $Params = @{}) {
	$script:rpcId++
	$request = [ordered]@{ jsonrpc='2.0'; id=$script:rpcId; method=$Method; params=$Params }
	$line = [ordered]@{ timestamp=[DateTime]::UtcNow.ToString('o'); request=$request }
	try {
		$response = Invoke-WebRequest -UseBasicParsing -Uri ("http://127.0.0.1:{0}/mcp" -f $script:port) `
			-Method Post -ContentType 'application/json' -Body ($request | ConvertTo-Json -Depth 40 -Compress) -TimeoutSec 30
		$parsed = $response.Content | ConvertFrom-Json
		$line.response = $parsed
		Add-Content -LiteralPath $script:rpcPath -Value ($line | ConvertTo-Json -Depth 40 -Compress)
		$errorProperty = $parsed.PSObject.Properties['error']
		if($null -ne $errorProperty -and $null -ne $errorProperty.Value) {
			throw "JSON-RPC error $($errorProperty.Value.code): $($errorProperty.Value.message)"
		}
		return $parsed
	} catch {
		$line.error = $_.Exception.Message
		Add-Content -LiteralPath $script:rpcPath -Value ($line | ConvertTo-Json -Depth 40 -Compress)
		throw
	}
}

function Get-ToolEnvelope($Response, [string]$Name, [switch]$AllowFailure) {
	if($null -eq $Response.result -or $null -eq $Response.result.content -or $Response.result.content.Count -lt 1) {
		throw "MCP '$Name' returned no result envelope."
	}
	$envelope = $Response.result.content[0].text | ConvertFrom-Json
	if(-not $envelope.ok -and -not $AllowFailure) {
		$errorProperty = $envelope.PSObject.Properties['error']
		$message = if($null -ne $errorProperty -and $null -ne $errorProperty.Value -and $errorProperty.Value.PSObject.Properties['message']) {
			[string]$errorProperty.Value.message
		} elseif($envelope.PSObject.Properties['message']) {
			[string]$envelope.message
		} else {
			'No error detail was returned.'
		}
		$status = if($envelope.PSObject.Properties['status']) { [string]$envelope.status } else { 'unknown status' }
		$approvalId = if($null -ne $envelope.data -and $envelope.data.PSObject.Properties['approvalId']) {
			[string]$envelope.data.approvalId
		} else { '' }
		$approvalText = if($approvalId) { " (approvalId=$approvalId; this driver does not approve high-impact operations)" } else { '' }
		throw "MCP '$Name' failed ($status)$approvalText`: $message"
	}
	return $envelope
}

function Invoke-Tool([string]$Name, $Arguments = @{}) {
	return Get-ToolEnvelope (Invoke-Rpc 'tools/call' ([ordered]@{name=$Name; arguments=$Arguments})) $Name
}

function Invoke-EditorStart {
	$prefs = [ordered]@{
		recentProjects = @(); theme = 'Dark'; externalScriptEditorCommand = ''; autoBuildScripts = $false
		editorMcpEnabled = $true; editorMcpWriteModeDefault = 'writes_enabled'
		editorMcpRecentSession = 'locomotion-authoring-acceptance'; editorMcpListenPort = $Port
		vulkanValidationLayers = $false; renderDebugDiagnostics = $false; rendererDiagnosticsPreferencesInitialized = $true
	}
	Write-JsonFile $preferencesPath $prefs
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $editor
	$start.WorkingDirectory = $editorDirectory
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$start.Environment['TERRAENGINE_EDITOR_PREFS_SESSION_PATH'] = $preferencesPath
	$start.Environment['TERRAENGINE_SUPPRESS_PROJECT_RECENTS'] = '1'
	$start.ArgumentList.Add('--project')
	$start.ArgumentList.Add($project)
	$script:process = [Diagnostics.Process]::Start($start)
	if($null -eq $script:process) { throw 'The installed editor process could not be started.' }
	# Asynchronous reads prevent a verbose application log from filling either
	# redirected pipe and blocking the editor while MCP requests are in flight.
	$script:stdoutTask = $script:process.StandardOutput.ReadToEndAsync()
	$script:stderrTask = $script:process.StandardError.ReadToEndAsync()
	$deadline = [DateTime]::UtcNow.AddSeconds(90)
	while([DateTime]::UtcNow -lt $deadline) {
		$script:process.Refresh()
		if($script:process.HasExited) { throw "Installed editor exited before MCP readiness (code $($script:process.ExitCode))." }
		try {
			$hello = Invoke-Rpc 'initialize' ([ordered]@{protocolVersion='2025-03-26'; clientInfo=[ordered]@{name='locomotion-authoring-acceptance'; version='1'}})
			if($hello.result.serverInfo.name -eq 'TerraEngine Editor') { return }
		} catch { Start-Sleep -Milliseconds 350 }
	}
	throw 'Installed editor MCP endpoint did not become ready within 90 seconds.'
}

function Stop-Editor {
	if($null -ne $script:process) {
		$script:process.Refresh()
		if(-not $script:process.HasExited) {
			$script:process.CloseMainWindow() | Out-Null
			if(-not $script:process.WaitForExit(15000)) {
				# A failed authoring step can leave a modal unsaved-scene prompt on
				# a locked desktop. Only terminate the exact process this driver
				# launched for the validated disposable fixture, never another editor.
				$ownedProcess = Get-CimInstance Win32_Process -Filter "ProcessId=$($script:process.Id)"
				if($null -eq $ownedProcess -or
					-not [string]::Equals($ownedProcess.ExecutablePath, $editor, [StringComparison]::OrdinalIgnoreCase) -or
					-not $ownedProcess.CommandLine.Contains($project, [StringComparison]::OrdinalIgnoreCase)) {
					throw 'Editor shutdown ownership could not be verified; no process was terminated.'
				}
				$script:failures.Add('Owned disposable editor required forced shutdown; normal teardown did not pass.')
				$script:process.Kill()
				if(-not $script:process.WaitForExit(15000)) { throw 'Owned disposable editor did not exit after termination.' }
			}
		}
		if($null -ne $script:stdoutTask) {
			# Process exit closes both redirected streams, so these awaits drain the
			# final bytes even when startup failed before MCP became ready.
			$stdoutText = $script:stdoutTask.GetAwaiter().GetResult()
			[IO.File]::WriteAllText($script:stdoutPath, $stdoutText, [Text.UTF8Encoding]::new($false))
		}
		if($null -ne $script:stderrTask) {
			$stderrText = $script:stderrTask.GetAwaiter().GetResult()
			[IO.File]::WriteAllText($script:stderrPath, $stderrText, [Text.UTF8Encoding]::new($false))
		}
		$script:process.Dispose()
		$script:process = $null
		$script:stdoutTask = $null
		$script:stderrTask = $null
	}
}

function Get-Component([string]$ObjectId, [string]$Component) {
	return (Invoke-Tool 'component.get' ([ordered]@{object=$ObjectId; component=$Component})).data.properties
}

function Wait-Receipt($Handle, [UInt64]$Producer, [UInt64]$Sequence, [UInt64]$Correlation, [int]$TimeoutSeconds = 12) {
	$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
	while([DateTime]::UtcNow -lt $deadline) {
		$reply = Invoke-Tool 'locomotion.receipt' ([ordered]@{handle=$Handle; producerId=$Producer; sequence=$Sequence; correlationId=$Correlation})
		$receipt = $reply.data.receipt
		if($receipt.state -in @('Applied','Adjusted','Rejected','Expired')) { return $receipt }
		Start-Sleep -Milliseconds 100
	}
	throw "Receipt $Producer/$Sequence/$Correlation did not become terminal."
}

$summary = [ordered]@{
	schemaVersion=1; status='running'; fixtureId='locomotion-060-v1'; startedUtc=[DateTime]::UtcNow.ToString('o')
	installRoot=$install; projectPath=$project; projectScene=$projectRelative; editorPath=$editor
	port=$Port; preferenceSession=$preferencesPath; capabilities=@(); cases=@(); failures=@(); unavailable=@()
	setup=[ordered]@{ status='not-run'; maximumSeconds=900; elapsedSeconds=$null; timingBasis='automation transaction through first observed locomotion speed' }
	evidence=[ordered]@{ rpcLog=$rpcPath; installedEditorProcess=$true; installedApplicationUsed=$true; inspectorMouseAutomation=$false }
}

if(-not (Test-FreeLoopbackPort $Port)) { throw "MCP loopback port $Port is occupied; choose a free explicit port." }

try {
	Invoke-EditorStart
	$listed = Invoke-Rpc 'tools/list'
	$toolNames = @($listed.result.tools | ForEach-Object { [string]$_.name })
	$summary.capabilities = $toolNames
	$requiredTools = @('project.state','scene.list_objects','scene.save','component.get','component.add',
		'object.create','transaction.execute','history.state','history.undo','history.redo','play.state','play.enter',
		'play.exit','locomotion.resolve','locomotion.command','locomotion.receipt','locomotion.inspect',
		'navigation.agent.bind','navigation.agent.destination.location','navigation.agent.status','navigation.agent.guidance',
		'prefab.create','prefab.instantiate','game_view.capture')
	$missingTools = @($requiredTools | Where-Object { $toolNames -notcontains $_ })
	if($missingTools.Count -gt 0) {
		foreach($id in @('editor.authoring.multi-edit','editor.authoring.rollback','editor.authoring.undo-redo',
			'editor.authoring.prefab-descendant-remap','editor.authoring.play-restoration','editor.authoring.setup-15m')) {
			Add-Case $id 'unavailable' @{} ("Required installed MCP capabilities are missing: " + ($missingTools -join ', '))
		}
		throw 'Installed editor does not advertise the complete required authoring capability surface.'
	}
	$state = Invoke-Tool 'project.state'
	if(-not $state.data.projectRoot -or [IO.Path]::GetFullPath([string]$state.data.projectRoot) -ne $project) {
		throw 'The installed editor opened a different project than the explicit disposable project path.'
	}
	# High-impact scene.open requires a separate user approval, which this
	# unattended driver must never bypass. The disposable project must therefore
	# launch with its generated Smoke scene selected as the startup document.
	# project.state reports the document stem, not the authored display name.
	if([string]$state.data.activeScene -ne 'Smoke') {
		throw "The disposable project opened '$($state.data.activeScene)' instead of generated scene 'Smoke'; correct its startup-scene configuration before running this driver."
	}

	$rows = (Invoke-Tool 'scene.list_objects' ([ordered]@{search='LOCO_VALIDATION_SMOKE'; includeComponents=$true; limit=16})).data.objects
	$sourceRows = @($rows | Where-Object { $_.name -eq 'LOCO_VALIDATION_SMOKE' -and $_.components -contains 'CharacterLocomotionComponent' })
	if($sourceRows.Count -ne 1) { throw 'Generated Smoke scene must contain exactly one configured LOCO_VALIDATION_SMOKE character.' }
	$sourceId = [string]$sourceRows[0].uuid
	$sourceInitial = Get-Component $sourceId 'CharacterLocomotionComponent'

	# Make a second genuine scene component, then edit both objects in one typed transaction.
	$created = Invoke-Tool 'object.create' ([ordered]@{name='LOCO_AUTHORING_MULTI_EDIT'; select=$false})
	$secondId = [string]$created.data.object.uuid
	if([string]::IsNullOrWhiteSpace($secondId)) { throw 'object.create did not return a stable UUID for the multi-edit target.' }
	[void](Invoke-Tool 'component.add' ([ordered]@{object=$secondId; component='CharacterLocomotionComponent'}))
	$originalSecond = Get-Component $secondId 'CharacterLocomotionComponent'
	$targetSpeed = 6.25
	$multi = Invoke-Tool 'transaction.execute' ([ordered]@{
		format='TerraSpatialTransactionV1'
		label='installed locomotion authoring multi-edit'
		correlationId='locomotion-authoring-multi-edit'; idempotencyKey='locomotion-authoring-multi-edit'
		operations=@(
			[ordered]@{kind='component.set_property'; target=$sourceId; component='CharacterLocomotionComponent'; property='maximumSpeed'; value=$targetSpeed},
			[ordered]@{kind='component.set_property'; target=$secondId; component='CharacterLocomotionComponent'; property='maximumSpeed'; value=$targetSpeed}
		)
	})
	$sourceAfterMulti = Get-Component $sourceId 'CharacterLocomotionComponent'
	$secondAfterMulti = Get-Component $secondId 'CharacterLocomotionComponent'
	if([double]$sourceAfterMulti.maximumSpeed -ne $targetSpeed -or [double]$secondAfterMulti.maximumSpeed -ne $targetSpeed) {
		throw 'Protected multi-object transaction did not publish both selected authored values.'
	}
	Add-Case 'editor.authoring.multi-edit' 'passed' ([ordered]@{transactionId=$multi.data.transactionId; objects=@($sourceId,$secondId); value=$targetSpeed; writeCount=$multi.data.appliedOperationCount})

	# A valid first write plus an invalid complete component candidate must leave both objects unchanged.
	$invalid = Invoke-Rpc 'tools/call' ([ordered]@{name='transaction.execute'; arguments=[ordered]@{
		format='TerraSpatialTransactionV1'
		label='installed locomotion rejected multi-edit rollback'; correlationId='locomotion-authoring-invalid'; idempotencyKey='locomotion-authoring-invalid'
		operations=@(
			[ordered]@{kind='component.set_property'; target=$sourceId; component='CharacterLocomotionComponent'; property='maximumSpeed'; value=9.0},
			[ordered]@{kind='component.set_property'; target=$secondId; component='CharacterLocomotionComponent'; property='braking'; value=0.0}
		)
	}})
	$invalidEnvelope = Get-ToolEnvelope $invalid 'transaction.execute' -AllowFailure
	$sourceAfterReject = Get-Component $sourceId 'CharacterLocomotionComponent'
	$secondAfterReject = Get-Component $secondId 'CharacterLocomotionComponent'
	if($invalidEnvelope.ok -or [double]$sourceAfterReject.maximumSpeed -ne $targetSpeed -or
		[double]$secondAfterReject.maximumSpeed -ne $targetSpeed -or [double]$secondAfterReject.braking -ne [double]$originalSecond.braking) {
		throw 'Invalid multi-edit was accepted or leaked a partial authored value.'
	}
	Add-Case 'editor.authoring.invalid-rollback' 'passed' ([ordered]@{rejected=$true; error=$invalidEnvelope.error; sourceSpeed=[double]$sourceAfterReject.maximumSpeed; secondSpeed=[double]$secondAfterReject.maximumSpeed})

	$history = Invoke-Tool 'history.state'
	$undo = Invoke-Tool 'history.undo' ([ordered]@{expectedSequence=[UInt64]$history.data.sequence})
	$sourceUndone = Get-Component $sourceId 'CharacterLocomotionComponent'
	$secondUndone = Get-Component $secondId 'CharacterLocomotionComponent'
	if([double]$sourceUndone.maximumSpeed -ne [double]$sourceInitial.maximumSpeed -or
		[double]$secondUndone.maximumSpeed -ne [double]$originalSecond.maximumSpeed) {
		throw 'Undo did not restore both pre-edit authored speed values.'
	}
	$redoState = Invoke-Tool 'history.state'
	$redo = Invoke-Tool 'history.redo' ([ordered]@{expectedSequence=[UInt64]$redoState.data.sequence})
	$sourceRedone = Get-Component $sourceId 'CharacterLocomotionComponent'
	$secondRedone = Get-Component $secondId 'CharacterLocomotionComponent'
	if([double]$sourceRedone.maximumSpeed -ne $targetSpeed -or [double]$secondRedone.maximumSpeed -ne $targetSpeed) {
		throw 'Redo did not restore both committed authored speed values.'
	}
	Add-Case 'editor.authoring.undo-redo' 'passed' ([ordered]@{undoHistoryId=$undo.data.historyId; redoHistoryId=$redo.data.historyId; restoredSpeed=$targetSpeed})

	# Build a real Self/descendant reference graph through the public editor APIs.
	$prefabRoot = Invoke-Tool 'object.create' ([ordered]@{name='LOCO_PREFAB_BINDING_ROOT'; select=$false})
	$prefabRootId = [string]$prefabRoot.data.object.uuid
	$prefabChild = Invoke-Tool 'object.create' ([ordered]@{name='LOCO_PREFAB_BINDING_ANIMATOR'; parent=$prefabRootId; select=$false})
	$prefabChildId = [string]$prefabChild.data.object.uuid
	[void](Invoke-Tool 'component.add' ([ordered]@{object=$prefabRootId; component='CharacterLocomotionComponent'}))
	[void](Invoke-Tool 'component.add' ([ordered]@{object=$prefabChildId; component='AnimatorComponent'}))
	$descendantConfig = Invoke-Tool 'transaction.execute' ([ordered]@{
		format='TerraSpatialTransactionV1'
		label='installed locomotion descendant binding'; correlationId='locomotion-prefab-descendant'; idempotencyKey='locomotion-prefab-descendant'
		operations=@(
			[ordered]@{kind='component.set_property'; target=$prefabRootId; component='CharacterLocomotionComponent'; property='animatorBinding'; value=2},
			[ordered]@{kind='component.set_property'; target=$prefabRootId; component='CharacterLocomotionComponent'; property='animatorObjectId'; value=$prefabChildId}
		)
	})
	$prefab = Invoke-Tool 'prefab.create' ([ordered]@{object=$prefabRootId; path='Assets/LocomotionAuthoringAcceptance.teprefab'})
	$prefabId = [string]$prefab.data.prefabId
	if([string]::IsNullOrWhiteSpace($prefabId)) { throw 'prefab.create did not return a stable prefab identity.' }
	$instance = Invoke-Tool 'prefab.instantiate' ([ordered]@{prefab=$prefabId; name='LOCO_PREFAB_BINDING_INSTANCE'})
	$instanceObjects = (Invoke-Tool 'scene.list_objects' ([ordered]@{search='LOCO_PREFAB_BINDING'; includeComponents=$true; limit=32})).data.objects
	$cloneRoots = @($instanceObjects | Where-Object { $_.name -eq 'LOCO_PREFAB_BINDING_INSTANCE' -and $_.components -contains 'CharacterLocomotionComponent' })
	$cloneChildren = @($instanceObjects | Where-Object { $_.name -eq 'LOCO_PREFAB_BINDING_ANIMATOR' -and $_.parentUuid -eq $cloneRoots[0].uuid })
	if($cloneRoots.Count -ne 1) { throw 'Prefab instantiate did not publish exactly one cloned parent.' }
	$cloneChildren = @($instanceObjects | Where-Object { $_.name -eq 'LOCO_PREFAB_BINDING_ANIMATOR' -and $_.parentUuid -eq $cloneRoots[0].uuid })
	if($cloneChildren.Count -ne 1) { throw 'Prefab instantiate did not publish exactly one cloned descendant animator.' }
	$cloneConfig = Get-Component ([string]$cloneRoots[0].uuid) 'CharacterLocomotionComponent'
	if([string]$cloneConfig.animatorObjectId -ne [string]$cloneChildren[0].uuid -or
		[string]$cloneConfig.animatorObjectId -eq $prefabChildId -or [uint32]$cloneConfig.animatorBinding -ne 2) {
		throw 'Prefab clone did not remap its authored descendant animator reference to the cloned child.'
	}
	Add-Case 'editor.authoring.prefab-descendant-remap' 'passed' ([ordered]@{prefabId=$prefabId; instanceId=$instance.data.instanceId; sourceAnimator=$prefabChildId; clonedAnimator=$cloneChildren[0].uuid; transactionId=$descendantConfig.data.transactionId})

	[void](Invoke-Tool 'scene.save')
	$authoredBytesHash = (Get-FileHash -LiteralPath $smokeScene -Algorithm SHA256).Hash.ToLowerInvariant()
	# Start the supplied-content setup clock before actual component authoring.
	# This character is created and configured through installed MCP tools, not
	# preauthored into the scene or moved through a test-only application seam.
	$script:setupStartMs = $script:latencyClock.Elapsed.TotalMilliseconds
	$setupObject = Invoke-Tool 'object.create' ([ordered]@{name='LOCO_AUTOMATED_SETUP_CHARACTER'; select=$false})
	$setupId = [string]$setupObject.data.object.uuid
	if([string]::IsNullOrWhiteSpace($setupId)) { throw 'The timed setup could not create a stable character identity.' }
	foreach($component in @('CharacterControllerComponent','NavigationAgentComponent','CharacterLocomotionComponent','AnimatorComponent')) {
		[void](Invoke-Tool 'component.add' ([ordered]@{object=$setupId; component=$component}))
	}
	# Read the current revision after authoring; component creation must not make
	# this optimistic transform write depend on a stale object-create response.
	$setupRows = @((Invoke-Tool 'scene.list_objects' ([ordered]@{search='LOCO_AUTOMATED_SETUP_CHARACTER'; limit=8})).data.objects | Where-Object { $_.uuid -eq $setupId })
	if($setupRows.Count -ne 1) { throw 'The setup character has no unique current transform revision.' }
	$setupConfig = Invoke-Tool 'transaction.execute' ([ordered]@{
		format='TerraSpatialTransactionV1'
		label='installed locomotion supplied-content setup'; correlationId='locomotion-setup-config'; idempotencyKey='locomotion-setup-config'
		operations=@(
			[ordered]@{kind='world.set_transform'; target=$setupId; observedTransformRevision=[UInt64]$setupRows[0].transformRevision; pose=[ordered]@{
				cell=[ordered]@{x='0';y='0';z='0'};offsetMetres=@(4.0,0.0,4.0)
				rotation=@(0.0,0.0,0.0,1.0); scale=@(1.0,1.0,1.0)
			}},
			# Match the frozen capsule and conservative navigation clearance. The
			# object origin is at its feet, not at the capsule centre.
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterControllerComponent'; property='radius'; value=0.35},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterControllerComponent'; property='height'; value=1.8},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterControllerComponent'; property='localCenter'; value=@(0.0,0.9,0.0)},
			[ordered]@{kind='component.set_property'; target=$setupId; component='NavigationAgentComponent'; property='heightMetres'; value=2.0},
			[ordered]@{kind='component.set_property'; target=$setupId; component='NavigationAgentComponent'; property='clearanceMarginMetres'; value=0.1},
			[ordered]@{kind='component.set_property'; target=$setupId; component='NavigationAgentComponent'; property='arrivalToleranceMetres'; value=0.25},
			[ordered]@{kind='component.set_property'; target=$setupId; component='AnimatorComponent'; property='controllerAssetId'; value=$controllerAssetId},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='maximumSpeed'; value=5.0},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='acceleration'; value=20.0},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='braking'; value=25.0},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='navigationBinding'; value=1},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='animatorBinding'; value=1},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='speedParameter'; value='Speed'},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='localDirectionXParameter'; value='DirectionX'},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='localDirectionZParameter'; value='DirectionZ'},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='groundedParameter'; value='Grounded'},
			[ordered]@{kind='component.set_property'; target=$setupId; component='CharacterLocomotionComponent'; property='movingParameter'; value='Moving'}
		)
	})
	$play = Invoke-Tool 'play.enter' ([ordered]@{expectedState='stopped'; correlationId='locomotion-authoring-play-enter'})
	$setupDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutMinutes * 60)
	$binding = $null
	while([DateTime]::UtcNow -lt $setupDeadline) {
		try { $binding = (Invoke-Tool 'locomotion.resolve' ([ordered]@{objectId=$setupId})).data; break } catch { Start-Sleep -Milliseconds 100 }
	}
	if($null -eq $binding) { throw 'Play Mode did not publish a current locomotion binding before the setup timeout.' }
	$navigationAgent = $null
	while([DateTime]::UtcNow -lt $setupDeadline) {
		try { $navigationAgent = (Invoke-Tool 'navigation.agent.bind' ([ordered]@{objectId=$setupId})).data.agent; break } catch { Start-Sleep -Milliseconds 100 }
	}
	if($null -eq $navigationAgent) { throw 'The setup character did not publish a generation-qualified NavigationAgent identity.' }
	$navigationStatus = (Invoke-Tool 'navigation.agent.status' ([ordered]@{agent=$navigationAgent})).data.status
	$destinationCorrelation = [UInt64]88206101
	$destinationResult = Invoke-Tool 'navigation.agent.destination.location' ([ordered]@{
		agent=$navigationAgent
		location=[ordered]@{worldId=[UInt64]$navigationAgent.worldId; worldGeneration=[UInt64]$navigationAgent.worldGeneration;
			position=[ordered]@{cell=[ordered]@{x='0';y='0';z='0'};offsetMetres=@(24.0,0.0,4.0)}}
		expectedConfigurationRevision=[UInt64]$navigationStatus.configurationRevision
		expectedIntentSequence=[UInt64]$navigationStatus.intentSequence
		producerOrderingToken=[UInt64]882061; correlationId=$destinationCorrelation
	})
	if($destinationResult.data.commitReceipt.state -ne 'Queued') {
		throw "Navigation destination did not queue: $($destinationResult.data.commitReceipt.state)/$($destinationResult.data.commitReceipt.reason)."
	}
	$destinationAdmitted = $false
	while([DateTime]::UtcNow -lt $setupDeadline) {
		$statusNow = (Invoke-Tool 'navigation.agent.status' ([ordered]@{agent=$navigationAgent})).data.status
		if([UInt64]$statusNow.intentSequence -gt [UInt64]$navigationStatus.intentSequence) { $destinationAdmitted = $true; break }
		Start-Sleep -Milliseconds 100
	}
	if(-not $destinationAdmitted) { throw 'Navigation destination did not become visible at its protected agent boundary.' }
	$producer = [UInt64]882060
	$sequence = [UInt64]1
	$correlation = [UInt64]88206001
	$acquire = [ordered]@{kind='Acquire'; handle=$binding.handle; expectedAuthorityEpoch=[UInt64]$binding.authorityEpoch;
		expectedConfigurationRevision=[UInt64]$binding.configurationRevision; ownerToken=[UInt64]0; producerId=$producer;
		sequence=$sequence; correlationId=$correlation; intentKind='Player'; mode='Velocity'}
	[void](Invoke-Tool 'locomotion.command' ([ordered]@{commands=@($acquire)}))
	$acquireReceipt = Wait-Receipt $binding.handle $producer $sequence $correlation
	if($acquireReceipt.state -notin @('Applied','Adjusted') -or [UInt64]$acquireReceipt.ownerToken -eq 0) {
		throw "Acquire did not apply: $($acquireReceipt.state)/$($acquireReceipt.reason)."
	}
	$sequence++; $correlation++
	$intent = [ordered]@{kind='FollowNavigation'; handle=$binding.handle; expectedAuthorityEpoch=[UInt64]$acquireReceipt.authorityEpoch;
		expectedConfigurationRevision=[UInt64]$binding.configurationRevision; ownerToken=[UInt64]$acquireReceipt.ownerToken;
		producerId=$producer; sequence=$sequence; correlationId=$correlation; intentKind='Navigation'; mode='Velocity';
		# Following is a persistent producer intent with an explicit speed cap;
		# omitted wire fields would mean zero speed for only one update.
		lifetime='UntilStopped'; requestedSpeed=5.0}
	[void](Invoke-Tool 'locomotion.command' ([ordered]@{commands=@($intent)}))
	$intentReceipt = Wait-Receipt $binding.handle $producer $sequence $correlation
	if($intentReceipt.state -notin @('Applied','Adjusted')) { throw "SetIntent did not apply: $($intentReceipt.state)/$($intentReceipt.reason)." }
	$motion = $null
	$guidance = $null
	while([DateTime]::UtcNow -lt $setupDeadline) {
		$observed = (Invoke-Tool 'locomotion.inspect' ([ordered]@{offset=0; maxItems=256})).data
		$motion = @($observed.observations | Where-Object { $_.handle.objectId -eq $setupId }) | Select-Object -First 1
		$guidance = (Invoke-Tool 'navigation.agent.guidance' ([ordered]@{agent=$navigationAgent})).data.guidance
		if($null -ne $motion -and [string]$guidance.state -eq 'Arrived' -and
			[double]$guidance.remainingDistanceMetres -le 0.25 -and [UInt64]$motion.physicalTick -gt 0) { break }
		Start-Sleep -Milliseconds 100
	}
	if($null -eq $motion -or [string]$guidance.state -ne 'Arrived' -or
		[double]$guidance.remainingDistanceMetres -gt 0.25 -or [UInt64]$motion.physicalTick -eq 0) {
		throw 'Installed Play Mode did not publish destination arrival within tolerance and a physical tick before the setup timeout.'
	}
	$script:setupFirstMotionMs = $script:latencyClock.Elapsed.TotalMilliseconds
	$elapsedSetup = ($script:setupFirstMotionMs - $script:setupStartMs) / 1000.0
	if($elapsedSetup -gt 900) { throw "The supplied-content automation workflow exceeded the 15-minute setup budget ($elapsedSetup seconds)." }
	$animationReady = [string]$motion.animationReason -eq 'None' -and [uint32]$motion.animationParameterError -eq 0
	if(-not $animationReady) { throw "The configured controller did not publish valid observed animation mapping status: $($motion.animationReason)/$($motion.animationParameterError)." }
	$captureRpc = Invoke-Rpc 'tools/call' ([ordered]@{name='game_view.capture'; arguments=[ordered]@{
		width=1280; height=720; format='image/png'; freshness='nextFrame'; includeOverlays=$false
	}})
	$imageBlocks = @($captureRpc.result.content | Where-Object { $_.type -eq 'image' -and $_.data })
	if($imageBlocks.Count -gt 0) {
		$screenshotPath = Join-Path $run 'play-mode-locomotion.png'
		[IO.File]::WriteAllBytes($screenshotPath, [Convert]::FromBase64String([string]$imageBlocks[0].data))
		Add-Case 'editor.authoring.play-screenshot' 'passed' ([ordered]@{path=$screenshotPath; mimeType=$imageBlocks[0].mimeType; bytes=(Get-Item -LiteralPath $screenshotPath).Length})
	} else {
		Add-Case 'editor.authoring.play-screenshot' 'unavailable' @{} 'The installed Game View capture response contained no image block.'
	}
	$exit = Invoke-Tool 'play.exit' ([ordered]@{expectedState='playing'; correlationId='locomotion-authoring-play-exit'})
	$sourceAfterPlay = Get-Component $setupId 'CharacterLocomotionComponent'
	if([double]$sourceAfterPlay.maximumSpeed -ne 5.0 -or [uint32]$sourceAfterPlay.animatorBinding -ne 1) { throw 'Play Stop restoration changed authored locomotion configuration.' }
	$afterStop = (Invoke-Tool 'locomotion.inspect' ([ordered]@{offset=0; maxItems=256})).data
	$stoppedMotion = @($afterStop.observations | Where-Object { $_.handle.objectId -eq $setupId }) | Select-Object -First 1
	$stoppedOwnerToken = if($null -ne $stoppedMotion) { [UInt64]$stoppedMotion.authority.ownerToken } else { [UInt64]0 }
	if($stoppedOwnerToken -ne 0) {
		throw 'Play Stop restored authored values but left locomotion runtime ownership active.'
	}
	Add-Case 'editor.authoring.play-restoration' 'passed' ([ordered]@{playEntry=$play.data; playExit=$exit.data; authoredMaximumSpeed=[double]$sourceAfterPlay.maximumSpeed; observedSpeed=[double]$motion.observedPlanarSpeed; physicalTick=[UInt64]$motion.physicalTick; animationReason=$motion.animationReason; animationParameterError=$motion.animationParameterError; stoppedAuthorityState=if($null -ne $stoppedMotion) { [string]$stoppedMotion.authority.state } else { 'no-current-observation' }; stoppedOwnerToken=$stoppedOwnerToken; setupTransactionId=$setupConfig.data.transactionId})
	$summary.setup = [ordered]@{status='passed'; maximumSeconds=900; elapsedSeconds=$elapsedSetup; timingBasis='component creation through navigation arrival with valid observed animation'; firstMotionPhysicalTick=[UInt64]$motion.physicalTick; arrivalDistanceMetres=[double]$guidance.remainingDistanceMetres; validAnimation=$animationReady}
	Add-Case 'editor.authoring.setup-15m' 'passed' ([ordered]@{elapsedSeconds=$elapsedSetup; maximumSeconds=900; receiptState=$intentReceipt.state; observedSpeed=[double]$motion.observedPlanarSpeed; arrivalState=$guidance.state; arrivalDistanceMetres=[double]$guidance.remainingDistanceMetres; validAnimation=$animationReady})

	[void](Invoke-Tool 'scene.save')
	Stop-Editor
	Invoke-EditorStart
	$reopened = (Invoke-Tool 'scene.list_objects' ([ordered]@{search='LOCO_AUTHORING_MULTI_EDIT'; includeComponents=$true; limit=8})).data.objects
	$reopenedRows = @($reopened | Where-Object { $_.uuid -eq $secondId })
	$reopenedConfig = if($reopenedRows.Count -eq 1) { Get-Component $secondId 'CharacterLocomotionComponent' } else { $null }
	if($null -eq $reopenedConfig -or [double]$reopenedConfig.maximumSpeed -ne $targetSpeed) {
		throw 'Saved scene did not reopen with the committed authored locomotion value.'
	}
	$reopenedHash = (Get-FileHash -LiteralPath $smokeScene -Algorithm SHA256).Hash.ToLowerInvariant()
	Add-Case 'editor.authoring.save-reopen' 'passed' ([ordered]@{objectId=$secondId; maximumSpeed=[double]$reopenedConfig.maximumSpeed; sourceSceneSha256Before=$authoredBytesHash; sourceSceneSha256After=$reopenedHash})
	# UI click-through is intentionally not inferred from MCP transactions.
	Add-Case 'editor.authoring.inspector-ui' 'unavailable' @{} 'The installed MCP exposes transactional multi-object edits but no Inspector mouse/UI automation surface.'
} catch {
	$script:failures.Add($_.Exception.Message)
	Add-Case 'editor.authoring.unhandled' 'failed' @{} $_.Exception.Message
} finally {
	try { Stop-Editor } catch { $script:failures.Add($_.Exception.Message) }
	$summary.finishedUtc = [DateTime]::UtcNow.ToString('o')
	$summary.cases = $script:cases.ToArray()
	$summary.failures = $script:failures.ToArray()
	$summary.unavailable = $script:unavailable.ToArray()
	$summary.status = if($script:failures.Count -gt 0) { 'failed' } elseif($script:unavailable.Count -gt 0) { 'incomplete' } else { 'passed' }
	Write-JsonFile $summaryPath $summary
}

Write-Output (Get-Content -LiteralPath $summaryPath -Raw)
if($summary.status -ne 'passed') { exit 1 }
