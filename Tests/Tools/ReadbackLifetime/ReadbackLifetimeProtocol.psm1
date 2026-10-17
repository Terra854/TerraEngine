# Pure receipt validation for the installed readback campaign. Importing this
# module never launches a process. Protocol-only fixtures prove parser behavior;
# they can never produce a real GPU/editor acceptance result.
Set-StrictMode -Version Latest

function Assert-ReadbackCondition([bool]$Condition, [string]$Code, [string]$Message) {
	if (!$Condition) { throw "[ReadbackProtocol:$Code] $Message" }
}

function Get-ReadbackField($Value, [string]$Name) {
	Assert-ReadbackCondition ($null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name]) 'field' "Missing field: $Name"
	return $Value.$Name
}

function Get-ReadbackInteger($Value, [string]$Name, [uint64]$Minimum = 0) {
	$n = Get-ReadbackField $Value $Name
	Assert-ReadbackCondition ($n -is [ValueType] -and $n -isnot [bool]) 'integer' "Non-numeric integer: $Name"
	$d = [double]$n
	Assert-ReadbackCondition (![double]::IsNaN($d) -and ![double]::IsInfinity($d) -and $d -eq [Math]::Truncate($d) -and $d -ge $Minimum -and $d -le 9007199254740991) 'integer' "Invalid integer: $Name"
	return [uint64]$n
}

function Assert-ReadbackBoolean($Value, [string]$Name, [bool]$Expected) {
	$v = Get-ReadbackField $Value $Name
	Assert-ReadbackCondition ($v -is [bool] -and $v -eq $Expected) 'boolean' "Wrong Boolean: $Name"
}

function Test-ReadbackBelow([string]$Path, [string]$Root) {
	return [IO.Path]::GetFullPath($Path).StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-ReadbackNoRedirect([string]$Path) {
	$cursor = [IO.Path]::GetFullPath($Path)
	while ($cursor) {
		if (Test-Path -LiteralPath $cursor) {
			Assert-ReadbackCondition (((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'path' 'Evidence paths cannot traverse reparse points.'
		}
		$cursor = [IO.Path]::GetDirectoryName($cursor)
	}
}

function Assert-ReadbackPin($Pin, [string]$Root) {
	$path = [string](Get-ReadbackField $Pin 'path')
	Assert-ReadbackCondition ([IO.Path]::IsPathRooted($path) -and (Test-ReadbackBelow $path $Root)) 'path' 'Evidence escaped its declared root.'
	Assert-ReadbackNoRedirect $path
	Assert-ReadbackCondition ([IO.File]::Exists($path)) 'missing-evidence' "Missing evidence: $path"
	$hash = [string](Get-ReadbackField $Pin 'sha256')
	Assert-ReadbackCondition ($hash -cmatch '^[0-9a-f]{64}$') 'hash' 'SHA-256 must be canonical lowercase hexadecimal.'
	$length = Get-ReadbackInteger $Pin 'bytes'
	Assert-ReadbackCondition ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $hash -and (Get-Item -LiteralPath $path).Length -eq $length) 'hash' "Evidence bytes changed: $path"
}

function Assert-ReadbackCommon($Receipt, $Context, [string]$Schema, [switch]$ProtocolOnly) {
	Assert-ReadbackCondition ((Get-ReadbackField $Receipt 'schemaVersion') -ceq $Schema) 'schema' 'Wrong readback receipt schema.'
	Assert-ReadbackCondition ((Get-ReadbackField $Receipt 'status') -ceq 'Passed') 'status' 'Missing prerequisites and unsuccessful runs are incomplete/failed, never acceptance.'
	$fixture = Get-ReadbackField $Receipt 'protocolFixture'
	Assert-ReadbackCondition ($fixture -is [bool]) 'boolean' 'protocolFixture must be Boolean.'
	Assert-ReadbackCondition (!$fixture -or $ProtocolOnly) 'synthetic' 'A protocol fixture cannot establish installed acceptance.'
	Assert-ReadbackBoolean $Context 'protocolContext' ([bool]$ProtocolOnly)
	Assert-ReadbackCondition ((Get-ReadbackField $Receipt 'runNonce') -ceq (Get-ReadbackField $Context 'runNonce')) 'nonce' 'Receipt belongs to a different launch.'
	$install = [string](Get-ReadbackField $Context 'installRoot')
	$work = [string](Get-ReadbackField $Context 'workRoot')
	Assert-ReadbackCondition ([IO.Path]::IsPathRooted($install) -and [IO.Path]::IsPathRooted($work) -and [IO.Path]::GetFullPath($install) -ine [IO.Path]::GetFullPath($work) -and !(Test-ReadbackBelow $work $install) -and !(Test-ReadbackBelow $install $work)) 'path' 'Install and work roots must be absolute and disjoint.'
	Assert-ReadbackNoRedirect $install; Assert-ReadbackNoRedirect $work
	Assert-ReadbackPin (Get-ReadbackField $Context 'manifestPin') $install
	Assert-ReadbackCondition ((Get-ReadbackField $Receipt 'installManifestSha256') -ceq $Context.manifestPin.sha256) 'manifest' 'Receipt install identity is stale.'
	$manifest = Get-Content -LiteralPath $Context.manifestPin.path -Raw | ConvertFrom-Json
	Assert-ReadbackCondition ((Get-ReadbackField $manifest 'schemaVersion') -ceq 'readback-lifetime-artifacts-v1') 'manifest' 'Wrong installed manifest schema.'
	$paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($pin in @(Get-ReadbackField $manifest 'files')) {
		$relative = [string](Get-ReadbackField $pin 'path')
		Assert-ReadbackCondition (![IO.Path]::IsPathRooted($relative) -and $paths.Add($relative)) 'manifest' 'Duplicate or absolute installed artifact path.'
		Assert-ReadbackPin ([pscustomobject]@{path=[IO.Path]::GetFullPath((Join-Path $install $relative));sha256=$pin.sha256;bytes=$pin.bytes}) $install
	}
	Assert-ReadbackCondition ($paths.Count -gt 0) 'manifest' 'Empty installed closure.'
	foreach ($required in @(Get-ReadbackField $Context 'requiredArtifacts')) { Assert-ReadbackCondition ($paths.Contains($required)) 'manifest' "Missing required installed artifact: $required" }
	$executable = [string](Get-ReadbackField $Receipt 'executablePath')
	Assert-ReadbackCondition ([IO.Path]::GetFullPath($executable) -ieq [IO.Path]::GetFullPath((Get-ReadbackField $Context 'executablePath')) -and (Test-ReadbackBelow $executable $install)) 'executable' 'Receipt was not produced by the expected installed executable.'
	$watchPin = Get-ReadbackField $Receipt 'watchdogPin'; Assert-ReadbackPin $watchPin $work
	$watch = Get-Content -LiteralPath $watchPin.path -Raw | ConvertFrom-Json
	Assert-ReadbackCondition ((Get-ReadbackInteger $watch 'pid' 1) -eq (Get-ReadbackInteger $Receipt 'processId' 1)) 'watchdog' 'Watchdog PID does not match launched fixture.'
	Assert-ReadbackCondition ((Get-ReadbackField $watch 'exitCode') -is [ValueType] -and $watch.exitCode -isnot [bool] -and $watch.exitCode -eq 0) 'watchdog' 'Child did not exit successfully.'
	Assert-ReadbackBoolean $watch 'timedOut' $false
	$pin = Get-ReadbackField $Receipt 'rawObservationsPin'; Assert-ReadbackPin $pin $work
	$raw = Get-Content -LiteralPath $pin.path -Raw | ConvertFrom-Json
	Assert-ReadbackCondition ((Get-ReadbackField $raw 'runNonce') -ceq $Receipt.runNonce) 'nonce' 'Raw observations belong to another launch.'
	Assert-ReadbackCondition ((ConvertTo-Json -InputObject @(Get-ReadbackField $raw 'observations') -Depth 100 -Compress) -ceq (ConvertTo-Json -InputObject @(Get-ReadbackField $Receipt 'observations') -Depth 100 -Compress)) 'raw-join' 'Receipt observations differ from pinned raw fixture output.'
	$iterations = Get-ReadbackInteger $Receipt 'iterations' 100
	Assert-ReadbackCondition ($iterations -eq (Get-ReadbackInteger $Context 'iterations' 100) -and @($Receipt.observations).Count -eq $iterations) 'iterations' 'Expected every requested iteration exactly once.'
	$seen = [Collections.Generic.HashSet[uint64]]::new()
	foreach ($row in @($Receipt.observations)) { $i=Get-ReadbackInteger $row 'iteration' 1; Assert-ReadbackCondition ($i -le $iterations -and $seen.Add($i)) 'iterations' 'Duplicate/out-of-range iteration.' }
	return $raw
}

function Test-ReadbackNativeReceipt($Receipt, $Context, [switch]$ProtocolOnly) {
	$raw = Assert-ReadbackCommon $Receipt $Context 'readback-lifetime-native-v1' -ProtocolOnly:$ProtocolOnly
	Assert-ReadbackCondition ((Get-ReadbackField $raw 'schemaVersion') -ceq 'readback-lifetime-native-observations-v1') 'raw-metadata' 'Native raw output has the wrong schema.'
	# The runner adds only installed/watchdog provenance. It must not replace
	# adapter, validation or ownership facts observed by the actual native process.
	foreach($key in @('processId','executablePath','iterations','iterationsPerScenario','protocolFixture','status','evidenceKind','validation','adapter','contextDestroyedAfterOperations','createdGateSemaphores','destroyedGateSemaphores')) {
		Assert-ReadbackCondition ((ConvertTo-Json -InputObject (Get-ReadbackField $raw $key) -Depth 100 -Compress) -ceq (ConvertTo-Json -InputObject (Get-ReadbackField $Receipt $key) -Depth 100 -Compress)) 'raw-metadata' "Native metadata differs from pinned process output: $key"
	}
	$perScenario=Get-ReadbackInteger $Context 'iterationsPerScenario' 100
	Assert-ReadbackCondition ((Get-ReadbackInteger $Receipt 'iterationsPerScenario' 100) -eq $perScenario -and $Context.iterations -eq 3UL*$perScenario) 'scenario-count' 'Native total must be three times the trusted per-scenario count.'
	Assert-ReadbackCondition ((Get-ReadbackField $Receipt 'evidenceKind') -ceq 'RealVulkan') 'native-kind' 'Simulation cannot pass the real GPU lane.'
	$validation = Get-ReadbackField $Receipt 'validation'
	Assert-ReadbackBoolean $validation 'actuallyEnabled' $true
	Assert-ReadbackBoolean $validation 'callbackInstalled' $true
	Assert-ReadbackBoolean $validation 'capturedThroughShutdown' $true
	Assert-ReadbackCondition (@(Get-ReadbackField $validation 'enabledLayers') -ccontains 'VK_LAYER_KHRONOS_validation') 'validation' 'Actual enabled validation layer name is absent.'
	Assert-ReadbackCondition (@(Get-ReadbackField $validation 'errors').Count -eq 0 -and (Get-ReadbackInteger $validation 'errorCount') -eq 0) 'validation' 'Native validation errors include initialization or teardown failures.'
	$adapter = Get-ReadbackField $Receipt 'adapter'
	foreach ($key in @('hardwareName','driverVersion','apiVersion')) { Assert-ReadbackCondition (![string]::IsNullOrWhiteSpace([string](Get-ReadbackField $adapter $key))) 'adapter' "Actual adapter identity is absent: $key" }
	Assert-ReadbackBoolean $Receipt 'contextDestroyedAfterOperations' $true
	$operations = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$scenarios = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$scenarioCounts=@{'timeout'=0;'resize'=0;'orderly-shutdown'=0}
	foreach ($row in @($Receipt.observations)) {
		$id=[string](Get-ReadbackField $row 'operationId'); $session=Get-ReadbackInteger $row 'sessionGeneration' 1
		Assert-ReadbackCondition (![string]::IsNullOrWhiteSpace($id) -and $operations.Add("${session}:$id")) 'identity' 'Duplicate or absent native operation identity.'
		$null=$scenarios.Add([string](Get-ReadbackField $row 'scenario'))
		Assert-ReadbackCondition ($row.scenario -cin @('timeout','resize','orderly-shutdown')) 'scenario' 'Unexpected native lifecycle scenario.'
		$scenarioCounts[$row.scenario]++
		Assert-ReadbackBoolean $row 'simulated' $false
		Assert-ReadbackBoolean $row 'forcedFenceObservation' $false
		$gate=Get-ReadbackField $row 'gate'
		Assert-ReadbackCondition ((Get-ReadbackField $gate 'kind') -ceq 'HostTimelineSemaphore' -and (Get-ReadbackField $gate 'waitCall') -ceq 'vkQueueSubmit' -and (Get-ReadbackInteger $gate 'initialValue') -eq 0 -and (Get-ReadbackInteger $gate 'waitValue') -eq 1 -and (Get-ReadbackInteger $gate 'waitStageMask') -eq 4096 -and (Get-ReadbackField $gate 'releaseCall') -ceq 'vkSignalSemaphore') 'gate' 'Readback must wait at TRANSFER on a real host-signalled timeline semaphore with initial value zero and wait value one.'
		Assert-ReadbackBoolean $gate 'timelineSemaphoreEnabled' $true
		Assert-ReadbackBoolean $gate 'timelineSemaphore' $true
		Assert-ReadbackCondition ((Get-ReadbackInteger $gate 'signalValue') -eq 1) 'gate' 'The actual host signal must release the submitted timeline wait value.'
		Assert-ReadbackBoolean $gate 'recordedBeforeCopy' $true; Assert-ReadbackBoolean $gate 'signaledBeforeSubmit' $false
		Assert-ReadbackBoolean $gate 'releaseGuardBeforeIdle' $true
		$events=@(Get-ReadbackField $row 'events'); $kinds=@('Submitted','NotReady','ClientTerminal','HostRelease','Completed','Retired')
		Assert-ReadbackCondition ($events.Count -eq $kinds.Count) 'events' 'Required ordered native observations are missing or duplicated.'
		$previous=0UL
		for($i=0;$i -lt $kinds.Count;$i++) {
			$e=$events[$i]; $sequence=Get-ReadbackInteger $e 'sequence' 1
			Assert-ReadbackCondition ((Get-ReadbackField $e 'kind') -ceq $kinds[$i] -and $sequence -gt $previous) 'events' 'Native event order is invalid.'; $previous=$sequence
			if($i -in @(0,1,3,4)) {
				$call=@{0='vkQueueSubmit';1='vkGetFenceStatus';3='vkSignalSemaphore';4='vkGetFenceStatus'}[$i]; $result=if($i -eq 1){1}else{0}
				# Shutdown may prove completion by a real successful idle wait after
				# releasing the event; normal and resize lanes require fence success.
				if($i -eq 4 -and $row.scenario -ceq 'orderly-shutdown') { $call='vkDeviceWaitIdle' }
				Assert-ReadbackCondition ((Get-ReadbackField $e 'call') -ceq $call -and (Get-ReadbackInteger $e 'result') -eq $result) 'vk-result' 'Expected an actual successful submit/release/completion and genuine VK_NOT_READY.'
			}
		}
		Assert-ReadbackCondition ((Get-ReadbackField $row 'terminalReason') -cin @('timeout','cancelled') -and (Get-ReadbackInteger $row 'terminalCountAfterDeadline') -eq 1 -and (Get-ReadbackInteger $row 'terminalCountAfterCompletion') -eq 1) 'terminal' 'Client delivery must occur once and remain terminal after native completion.'
		$retained=Get-ReadbackField $row 'retainedAfterTerminal'
		$earlyDestroyed=Get-ReadbackField $row 'destroyedAfterTerminal'
		$destroyed=Get-ReadbackField $row 'destroyedAfterCompletion'
		foreach($resource in @('stagingBuffer','allocation','commandBuffer','fence')) {
			Assert-ReadbackCondition ((Get-ReadbackInteger $retained $resource) -eq 1 -and (Get-ReadbackInteger $earlyDestroyed $resource) -eq 0 -and (Get-ReadbackInteger $destroyed $resource) -eq 1) 'ownership' "Native owner retired early or more than once: $resource"
		}
		Assert-ReadbackBoolean $row 'sourceBlockedAfterTerminal' $true; Assert-ReadbackBoolean $row 'sourceBlockedAfterCompletion' $false
		Assert-ReadbackBoolean $row 'commandPoolAliveThroughRetirement' $true
		Assert-ReadbackBoolean $row 'sourceImageDestroyedBeforeRetirement' $false
		if($row.scenario -ceq 'resize') {
			Assert-ReadbackBoolean $row 'resizeAfterRetirement' $true
			Assert-ReadbackCondition ((Get-ReadbackInteger $row 'resizeSequence') -gt $previous) 'resize' 'Real resize must follow gated readback retirement.'
		}
	}
	Assert-ReadbackCondition ((Get-ReadbackInteger $Receipt 'createdGateSemaphores') -eq (Get-ReadbackInteger $Receipt 'destroyedGateSemaphores') -and $Receipt.createdGateSemaphores -gt 0) 'gate-destruction' 'Fixture-owned timeline semaphores must retire once at owning context teardown.'
	foreach($required in @('timeout','resize','orderly-shutdown')) {
		Assert-ReadbackCondition ($scenarios.Contains($required)) 'scenario' "Missing actual lifecycle scenario: $required"
	}
	foreach($required in @('timeout','resize','orderly-shutdown')) {
		Assert-ReadbackCondition ($scenarioCounts[$required] -eq $perScenario) 'scenario-count' "Expected the exact trusted count for native scenario: $required"
	}
	return [pscustomobject]@{schemaVersion='readback-lifetime-validation-v1';status='Passed';lane='Native';iterations=$Receipt.iterations;protocolOnly=[bool]$ProtocolOnly;nativeAcceptance=(!$ProtocolOnly)}
}

function Assert-ReadbackEditorCapture($row, $Context, $requests) {
	$id=[string](Get-ReadbackField $row 'requestId')
	Assert-ReadbackCondition (![string]::IsNullOrWhiteSpace($id) -and $requests.Add($id) -and (Get-ReadbackField $row 'responseId') -ceq $id -and (Get-ReadbackInteger $row 'responseCount') -eq 1) 'response' 'Editor response identity/count mismatch.'
	Assert-ReadbackCondition ((Get-ReadbackField $row 'correlationId') -ceq $id) 'correlation' 'Capture correlation was not preserved.'
	Assert-ReadbackCondition ((Get-ReadbackField $row 'operation') -cin @('game_view.capture','camera.capture_custom')) 'operation' 'Unexpected editor capture operation.'
	Assert-ReadbackBoolean $row 'backendReadback' $true; Assert-ReadbackBoolean $row 'deterministicFallback' $false
	Assert-ReadbackPin (Get-ReadbackField $row 'imagePin') $Context.workRoot
	Assert-ReadbackPin (Get-ReadbackField $row 'rpcPin') $Context.workRoot
	$rpc=Get-Content -LiteralPath $row.rpcPin.path -Raw | ConvertFrom-Json
	Assert-ReadbackCondition ($rpc.request.id -ceq $id -and $rpc.response.id -ceq $id -and $rpc.request.method -ceq 'tools/call' -and $rpc.request.params.name -ceq $row.operation) 'rpc' 'Pinned RPC does not match the exact request/response/capture operation.'
	Assert-ReadbackCondition ($rpc.request.params.arguments.width -eq $row.requestedWidth -and $rpc.request.params.arguments.height -eq $row.requestedHeight -and $rpc.request.params.arguments.correlationId -ceq $id) 'rpc' 'Observed dimensions/correlation differ from actual RPC arguments.'
	$images=@($rpc.response.result.content | Where-Object type -CEQ 'image')
	$texts=@($rpc.response.result.content | Where-Object type -CEQ 'text')
	Assert-ReadbackCondition ($images.Count -eq 1 -and $texts.Count -eq 1 -and $images[0].mimeType -ceq 'image/png') 'rpc' 'Pinned response must contain one actual PNG and one text envelope.'
	$transmitted=[Convert]::FromBase64String($images[0].data)
	$transmittedHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($transmitted)).ToLowerInvariant()
	Assert-ReadbackCondition ($transmittedHash -ceq $row.imagePin.sha256) 'image-join' 'Decoded input differs from actual transmitted MCP bytes.'
	$envelope=$texts[0].text | ConvertFrom-Json
	Assert-ReadbackBoolean $envelope 'ok' $true
	Assert-ReadbackBoolean $envelope.data 'backendReadback' $true; Assert-ReadbackBoolean $envelope.data 'deterministicFallback' $false
	Assert-ReadbackCondition ($envelope.correlationId -ceq $id -and $envelope.data.width -eq $row.requestedWidth -and $envelope.data.height -eq $row.requestedHeight) 'rpc' 'Native envelope dimensions/correlation do not match request.'
	$decoded=Get-ReadbackField $row 'decoder'
	Assert-ReadbackPin (Get-ReadbackField $row 'decoderReceiptPin') $Context.workRoot
	$rawDecoder=Get-Content -LiteralPath $row.decoderReceiptPin.path -Raw | ConvertFrom-Json
	Assert-ReadbackCondition ((ConvertTo-Json $rawDecoder -Depth 100 -Compress) -ceq (ConvertTo-Json $decoded -Depth 100 -Compress)) 'decode-join' 'Decoder result differs from pinned independent process output.'
	Assert-ReadbackCondition ([IO.Path]::GetFullPath($decoded.executablePath) -ieq [IO.Path]::GetFullPath((Get-ReadbackField $Context 'decoderExecutablePath')) -and (Test-ReadbackBelow $decoded.executablePath $Context.installRoot) -and (Get-ReadbackInteger $decoded 'processId' 1) -gt 0 -and (Get-ReadbackInteger $decoded 'exitCode') -eq 0) 'decode-process' 'Independent decoder must be the installed executable and exit successfully.'
	Assert-ReadbackCondition ((Get-ReadbackField $decoded 'imageSha256') -ceq $row.imagePin.sha256 -and (Get-ReadbackField $decoded 'status') -ceq 'Decoded' -and (Get-ReadbackInteger $decoded 'width' 1) -eq (Get-ReadbackInteger $row 'requestedWidth' 1) -and (Get-ReadbackInteger $decoded 'height' 1) -eq (Get-ReadbackInteger $row 'requestedHeight' 1)) 'decode' 'Independent decoded bytes do not match transmitted PNG/request extent.'
	Assert-ReadbackBoolean $decoded 'checksumsVerified' $true
	if($row.operation -ceq 'game_view.capture') {
		Assert-ReadbackCondition ($rpc.request.params.arguments.freshness -ceq 'nextFrame') 'target' 'Game View must request the same dedicated nextFrame target.'
	}
	$target=Get-ReadbackField $row 'backendTarget'
	Assert-ReadbackCondition ((ConvertTo-Json -InputObject (Get-ReadbackField $envelope.data 'backendTarget') -Compress) -ceq (ConvertTo-Json -InputObject $target -Compress)) 'target-join' 'Backend target observation differs from pinned actual MCP response.'
	Assert-ReadbackBoolean $target 'dedicatedCaptureTarget' $true
	$null=Get-ReadbackInteger $target 'rendererSessionGeneration' 1
	$null=Get-ReadbackInteger $target 'targetGeneration' 1
	Assert-ReadbackCondition ((Get-ReadbackInteger $target 'sourceWidth' 1) -eq $row.requestedWidth -and (Get-ReadbackInteger $target 'sourceHeight' 1) -eq $row.requestedHeight) 'target' 'Actual source target extent must match requested and independently decoded pixels.'
	return $target
}

function Test-ReadbackEditorReceipt($Receipt, $Context, [switch]$ProtocolOnly) {
	$raw=Assert-ReadbackCommon $Receipt $Context 'readback-lifetime-editor-v1' -ProtocolOnly:$ProtocolOnly
	foreach($key in @('endpointOwnershipVerified','isolatedProjectVerified','authoredObjectsUnchanged','sceneFileUnchanged','sceneDirtyUnchanged','normalClose')) { Assert-ReadbackBoolean $Receipt $key $true }
	$before=Get-ReadbackField $Receipt 'sceneBeforePin'; $after=Get-ReadbackField $Receipt 'sceneAfterPin'
	Assert-ReadbackPin $before $Context.workRoot; Assert-ReadbackPin $after $Context.workRoot
	Assert-ReadbackCondition ([IO.Path]::GetFullPath($before.path) -ieq [IO.Path]::GetFullPath($after.path) -and $before.sha256 -ceq $after.sha256 -and $before.bytes -eq $after.bytes) 'scene' 'Read-only captures changed the authored scene file.'
	foreach($key in @('warmup','resizeCount','normalCloseCount','capturesBeforeNormalClose')) {
		Assert-ReadbackCondition ((ConvertTo-Json -InputObject (Get-ReadbackField $raw $key) -Depth 100 -Compress) -ceq (ConvertTo-Json -InputObject (Get-ReadbackField $Receipt $key) -Depth 100 -Compress)) 'raw-metadata' "Editor session metadata differs from raw loopback output: $key"
	}
	Assert-ReadbackCondition ((Get-ReadbackInteger $Receipt 'resizeCount' 100) -eq $Context.iterations -and (Get-ReadbackInteger $Receipt 'normalCloseCount') -eq 1 -and (Get-ReadbackInteger $Receipt 'capturesBeforeNormalClose') -eq $Context.iterations) 'editor-session' 'Require one normal close after all captures and one observed resize per capture, not repeated shutdown claims.'
	$requests=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$operations=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$previous=Assert-ReadbackEditorCapture (Get-ReadbackField $Receipt 'warmup') $Context $requests
	foreach($row in @($Receipt.observations)) {
		$target=Assert-ReadbackEditorCapture $row $Context $requests
		$null=$operations.Add($row.operation)
		Assert-ReadbackCondition ($target.rendererSessionGeneration -eq $previous.rendererSessionGeneration -and $target.targetGeneration -gt $previous.targetGeneration) 'target' 'Every real target resize must advance generation within the same renderer session.'
		Assert-ReadbackCondition ($target.sourceWidth -ne $previous.sourceWidth -or $target.sourceHeight -ne $previous.sourceHeight) 'target' 'Every capture must use a different actual target extent from its warmup/predecessor.'
		$previous=$target
	}
	Assert-ReadbackCondition ($operations.Contains('game_view.capture') -and $operations.Contains('camera.capture_custom')) 'operation' 'Both read-only camera capture operations must be observed.'
	return [pscustomobject]@{schemaVersion='readback-lifetime-validation-v1';status='Passed';lane='Editor';iterations=$Receipt.iterations;resizeCount=$Receipt.resizeCount;normalCloseCount=1;protocolOnly=[bool]$ProtocolOnly;nativeAcceptance=(!$ProtocolOnly)}
}

Export-ModuleMember -Function Test-ReadbackNativeReceipt,Test-ReadbackEditorReceipt
