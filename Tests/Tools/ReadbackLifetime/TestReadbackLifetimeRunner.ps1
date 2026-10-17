<#
Pure protocol regressions; no Vulkan fixture or editor is launched.
Prerequisite: bundled PowerShell 7. Invoke with a fresh disposable output root:
pwsh -File Tests/Tools/ReadbackLifetime/TestReadbackLifetimeRunner.ps1 -WorkRoot C:/Temp/readback-protocol-fresh
All positive receipts remain explicitly synthetic and cannot pass real acceptance.
#>
[CmdletBinding()]
param([string]$WorkRoot = (Join-Path ([IO.Path]::GetTempPath()) ('TerraReadbackProtocol-' + [guid]::NewGuid().ToString('N'))))
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'ReadbackLifetimeProtocol.psm1') -Force
$work=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $work){throw 'Protocol WorkRoot must be fresh.'}
$null=[IO.Directory]::CreateDirectory($work)
$script:checks=0

function Write-ProtocolJson([string]$Path,$Value) {
	[IO.File]::WriteAllText($Path,(ConvertTo-Json -InputObject $Value -Depth 100)+"`n",[Text.UTF8Encoding]::new($false))
}
function Get-ProtocolPin([string]$Path) {
	return [pscustomobject]@{path=$Path;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $Path).Length}
}
function Copy-ProtocolValue($Value) { return ConvertTo-Json -InputObject $Value -Depth 100 | ConvertFrom-Json }
function Invoke-ProtocolCheck([string]$Name,[scriptblock]$Body) {
	& $Body
	$script:checks++
	Write-Output "[PASS] $Name"
}
function Assert-ProtocolReject([string]$Code,[scriptblock]$Body) {
	$caught=$false
	try { & $Body | Out-Null }
	catch {
		# A missing command, import failure or accidental runtime exception is not
		# the intended failing assertion and must fail this regression instead.
		if(!$_.Exception.Message.Contains("[ReadbackProtocol:$Code]")){throw}
		$caught=$true
	}
	if(!$caught){throw "Expected protocol assertion: $Code"}
}

$install=Join-Path $work 'protocol-only-install';$evidence=Join-Path $work 'observations'
$null=[IO.Directory]::CreateDirectory($install);$null=[IO.Directory]::CreateDirectory($evidence)
$fixture=Join-Path $install 'ReadbackLifetimeFixture.exe';$editor=Join-Path $install 'TerraEngine-Editor.exe'
foreach($path in @($fixture,$editor)){[IO.File]::WriteAllText($path,'PROTOCOL ONLY: not an executable or installed runtime')}
$files=@(foreach($path in @($fixture,$editor)){$pin=Get-ProtocolPin $path;[pscustomobject]@{path=[IO.Path]::GetFileName($path);sha256=$pin.sha256;bytes=$pin.bytes}})
$manifest=Join-Path $install 'readback-lifetime-artifacts.json'
Write-ProtocolJson $manifest ([pscustomobject]@{schemaVersion='readback-lifetime-artifacts-v1';files=$files})
$watchPath=Join-Path $evidence 'watchdog.json'
$healthyWatch=[pscustomobject]@{pid=7001;exitCode=0;timedOut=$false;elapsedMilliseconds=100}
Write-ProtocolJson $watchPath $healthyWatch
$image=Join-Path $evidence 'protocol-control.png';Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Fixtures/protocol-control.png') -Destination $image
$imagePin=Get-ProtocolPin $image
$secondImage=Join-Path $evidence 'protocol-control-2x3.png';Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Fixtures/protocol-control-2x3.png') -Destination $secondImage
$native=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Fixtures/valid-native-protocol.json') -Raw | ConvertFrom-Json
$editorReceipt=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Fixtures/valid-editor-protocol.json') -Raw | ConvertFrom-Json
$nonce=[guid]::NewGuid().ToString('N')
foreach($receipt in @($native,$editorReceipt)) {
	$receipt.runNonce=$nonce;$receipt.installManifestSha256=(Get-ProtocolPin $manifest).sha256
	$receipt.watchdogPin=Get-ProtocolPin $watchPath
}
$native.executablePath=$fixture;$editorReceipt.executablePath=$editor
$scene=Join-Path $evidence 'protocol-only.tescene';[IO.File]::WriteAllText($scene,'PROTOCOL ONLY authored scene bytes')
$editorReceipt | Add-Member -NotePropertyName sceneBeforePin -NotePropertyValue (Get-ProtocolPin $scene)
$editorReceipt | Add-Member -NotePropertyName sceneAfterPin -NotePropertyValue (Get-ProtocolPin $scene)

# Each editor iteration retains actual JSON-RPC-shaped bytes plus a separate
# decoder-shaped output. Their synthetic origin is retained on the outer receipt.
foreach($row in @($editorReceipt.warmup)+@($editorReceipt.observations)) {
	$sourceImage=if($row.requestedWidth -eq 3){$image}else{$secondImage}
	$row.imagePin=Get-ProtocolPin $sourceImage
	$row.decoder.imageSha256=$row.imagePin.sha256;$row.decoder.executablePath=$fixture
	$decoderPath=Join-Path $evidence ("decoder-"+$row.iteration+'.json')
	Write-ProtocolJson $decoderPath $row.decoder;$row.decoderReceiptPin=Get-ProtocolPin $decoderPath
	$envelope=[pscustomobject]@{ok=$true;correlationId=$row.requestId;data=[pscustomobject]@{backendReadback=$true;deterministicFallback=$false;width=$row.requestedWidth;height=$row.requestedHeight;backendTarget=$row.backendTarget}}
	$rpc=[pscustomobject]@{request=[pscustomobject]@{id=$row.requestId;method='tools/call';params=[pscustomobject]@{name=$row.operation;arguments=[pscustomobject]@{width=$row.requestedWidth;height=$row.requestedHeight;correlationId=$row.requestId;freshness='nextFrame'}}};response=[pscustomobject]@{id=$row.requestId;result=[pscustomobject]@{content=@([pscustomobject]@{type='text';text=(ConvertTo-Json $envelope -Depth 30 -Compress)},[pscustomobject]@{type='image';mimeType='image/png';data=[Convert]::ToBase64String([IO.File]::ReadAllBytes($sourceImage))})}}}
	$rpcPath=Join-Path $evidence ("rpc-"+$row.iteration+'.json');Write-ProtocolJson $rpcPath $rpc;$row.rpcPin=Get-ProtocolPin $rpcPath
}

function Save-ProtocolRaw($Receipt) {
	$rawPath=Join-Path $evidence ($Receipt.schemaVersion+'-raw.json')
	$raw=[pscustomobject]@{runNonce=$Receipt.runNonce;observations=@($Receipt.observations)}
	if($Receipt.schemaVersion -ceq 'readback-lifetime-native-v1') {
		$raw | Add-Member -NotePropertyName schemaVersion -NotePropertyValue 'readback-lifetime-native-observations-v1'
		foreach($key in @('processId','executablePath','iterations','iterationsPerScenario','protocolFixture','status','evidenceKind','validation','adapter','contextDestroyedAfterOperations','createdGateSemaphores','destroyedGateSemaphores')) {
			$raw | Add-Member -NotePropertyName $key -NotePropertyValue $Receipt.$key
		}
	}
	elseif($Receipt.schemaVersion -ceq 'readback-lifetime-editor-v1') {
		foreach($key in @('warmup','resizeCount','normalCloseCount','capturesBeforeNormalClose')) {$raw | Add-Member -NotePropertyName $key -NotePropertyValue $Receipt.$key}
	}
	Write-ProtocolJson $rawPath $raw
	$Receipt.rawObservationsPin=Get-ProtocolPin $rawPath
}

function Sync-ProtocolRaw($Receipt) {
	if($Receipt.schemaVersion -ceq 'readback-lifetime-editor-v1') {
		foreach($row in @($Receipt.warmup)+@($Receipt.observations)) {
			Write-ProtocolJson $row.decoderReceiptPin.path $row.decoder
			$row.decoderReceiptPin=Get-ProtocolPin $row.decoderReceiptPin.path
		}
	}
	Save-ProtocolRaw $Receipt
}
Sync-ProtocolRaw $native;Sync-ProtocolRaw $editorReceipt
$common=[pscustomobject]@{protocolContext=$true;runNonce=$nonce;installRoot=$install;workRoot=$evidence;manifestPin=(Get-ProtocolPin $manifest);requiredArtifacts=@('ReadbackLifetimeFixture.exe','TerraEngine-Editor.exe');iterations=300;iterationsPerScenario=100;executablePath=$fixture;decoderExecutablePath=$fixture}
function Test-ProtocolLane($Receipt,[string]$Lane,[switch]$RealAcceptance) {
	$context=Copy-ProtocolValue $common
	if($Lane -ceq 'Editor'){$context.executablePath=$editor;$context.iterations=100;return Test-ReadbackEditorReceipt $Receipt $context -ProtocolOnly:(!$RealAcceptance)}
	return Test-ReadbackNativeReceipt $Receipt $context -ProtocolOnly:(!$RealAcceptance)
}

foreach($lane in @('Native','Editor')) {
	$sample=if($lane -ceq 'Native'){$native}else{$editorReceipt}
	$count=if($lane -ceq 'Native'){300}else{100}
	Invoke-ProtocolCheck "$lane-valid-$count-observation-shape-is-protocol-only" {
		$result=Test-ProtocolLane $sample $lane
		if($result.iterations -ne $count -or $result.nativeAcceptance -or !$result.protocolOnly){throw 'Protocol validation falsely claimed acceptance.'}
	}
	Invoke-ProtocolCheck "$lane-synthetic-corpus-cannot-pass-real-acceptance" { Assert-ProtocolReject 'synthetic' { Test-ProtocolLane $sample $lane -RealAcceptance } }
	Invoke-ProtocolCheck "$lane-removing-receipt-fixture-label-cannot-promote-mock-context" {
		$r=Copy-ProtocolValue $sample;$r.protocolFixture=$false
		Assert-ProtocolReject 'boolean' { Test-ProtocolLane $r $lane -RealAcceptance }
	}
}

function Set-ProtocolMutation($Object,$Case) {
	$parts=$Case.path.Split('.');$cursor=$Object
	for($i=0;$i -lt $parts.Length-1;$i++) {
		$key=$parts[$i]
		$cursor=if($cursor -is [Array]){$cursor[[int]$key]}else{$cursor.$key}
	}
	$key=$parts[-1]
	if($cursor -is [Array]){$cursor[[int]$key]=$Case.value}
	elseif($Case.delete){$cursor.PSObject.Properties.Remove($key)}
	else{$cursor.$key=$Case.value}
}
$corpus=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Fixtures/invalid-receipts.json') -Raw | ConvertFrom-Json
foreach($case in @($corpus.cases)) {
	Invoke-ProtocolCheck $case.caseId {
		$r=Copy-ProtocolValue $(if($case.lane -ceq 'Native'){$native}else{$editorReceipt})
		Sync-ProtocolRaw $r
		Set-ProtocolMutation $r $case
		# Keep raw records in agreement when testing a deeper invariant. Separate
		# cases deliberately leave them stale to prove the provenance join rejects it.
		if($case.synchronizeRaw){Sync-ProtocolRaw $r}
		Assert-ProtocolReject $case.expectedCode { Test-ProtocolLane $r $case.lane }
	}
}

foreach($fault in @('watchdog-killed','watchdog-nonzero','watchdog-boolean-exit')) {
	Invoke-ProtocolCheck $fault {
		$watch=Copy-ProtocolValue $healthyWatch
		if($fault -ceq 'watchdog-killed'){$watch.timedOut=$true;$watch.exitCode=124}
		elseif($fault -ceq 'watchdog-boolean-exit'){$watch.exitCode=$false}
		else{$watch.exitCode=1}
		Write-ProtocolJson $watchPath $watch
		$r=Copy-ProtocolValue $native;$r.watchdogPin=Get-ProtocolPin $watchPath;Sync-ProtocolRaw $r
		Assert-ProtocolReject 'watchdog' { Test-ProtocolLane $r Native }
	}
}
Write-ProtocolJson $watchPath $healthyWatch
Invoke-ProtocolCheck 'missing-required-installed-closure-is-not-a-pass' {
	$c=Copy-ProtocolValue $common;$c.requiredArtifacts+=,'missing-runtime.dll'
	Sync-ProtocolRaw $native
	Assert-ProtocolReject 'manifest' { Test-ReadbackNativeReceipt $native $c -ProtocolOnly }
}
Invoke-ProtocolCheck 'work-root-overlap-is-not-a-pass' {
	$c=Copy-ProtocolValue $common;$c.workRoot=$install
	Assert-ProtocolReject 'path' { Test-ReadbackNativeReceipt $native $c -ProtocolOnly }
}

foreach($fault in @('missing-actual-event','missing-resize-scenario')) {
	Invoke-ProtocolCheck $fault {
		$r=Copy-ProtocolValue $native;$r.watchdogPin=Get-ProtocolPin $watchPath
		if($fault -ceq 'missing-actual-event'){$r.observations[0].events=@($r.observations[0].events | Where-Object kind -CNE 'NotReady');$code='events'}
		else{foreach($row in @($r.observations)){if($row.scenario -ceq 'resize'){$row.scenario='timeout'}};$code='scenario'}
		Sync-ProtocolRaw $r;Assert-ProtocolReject $code { Test-ProtocolLane $r Native }
	}
}
Invoke-ProtocolCheck 'Native-uneven-scenario-distribution' {
	$r=Copy-ProtocolValue $native;$r.watchdogPin=Get-ProtocolPin $watchPath
	$r.observations[1].scenario='timeout';Sync-ProtocolRaw $r
	Assert-ProtocolReject 'scenario-count' { Test-ProtocolLane $r Native }
}
Invoke-ProtocolCheck 'Native-trusted-perScenario-underfloor' {
	$r=Copy-ProtocolValue $native;$r.watchdogPin=Get-ProtocolPin $watchPath;Sync-ProtocolRaw $r
	$c=Copy-ProtocolValue $common;$c.iterationsPerScenario=99
	Assert-ProtocolReject 'integer' { Test-ReadbackNativeReceipt $r $c -ProtocolOnly }
}
foreach($key in @('processId','executablePath','adapter','validation')) {
	Invoke-ProtocolCheck "Native-pinned-raw-$key-forgery" {
		$r=Copy-ProtocolValue $native;$r.watchdogPin=Get-ProtocolPin $watchPath;Sync-ProtocolRaw $r
		$raw=Get-Content -LiteralPath $r.rawObservationsPin.path -Raw | ConvertFrom-Json
		if($key -ceq 'processId'){$raw.processId=7009}
		elseif($key -ceq 'executablePath'){$raw.executablePath='C:/foreign.exe'}
		elseif($key -ceq 'adapter'){$raw.adapter.hardwareName='FORGED OTHER ADAPTER'}
		else{$raw.validation.actuallyEnabled=$false}
		Write-ProtocolJson $r.rawObservationsPin.path $raw;$r.rawObservationsPin=Get-ProtocolPin $r.rawObservationsPin.path
		Assert-ProtocolReject 'raw-metadata' { Test-ProtocolLane $r Native }
	}
}
foreach($fault in @('different-transmitted-image','different-request-width','synthetic-raw-envelope','stale-decoder-output')) {
	Invoke-ProtocolCheck $fault {
		$r=Copy-ProtocolValue $editorReceipt;$r.watchdogPin=Get-ProtocolPin $watchPath
		Sync-ProtocolRaw $r
		$row=$r.observations[0]
		$changedPath=if($fault -ceq 'stale-decoder-output'){$row.decoderReceiptPin.path}else{$row.rpcPin.path}
		$original=[IO.File]::ReadAllBytes($changedPath)
		try {
			$data=Get-Content -LiteralPath $changedPath -Raw | ConvertFrom-Json
			if($fault -ceq 'different-transmitted-image'){$data.response.result.content[1].data=[Convert]::ToBase64String([byte[]]@(1,2,3));$code='image-join'}
			elseif($fault -ceq 'different-request-width'){$data.request.params.arguments.width=8;$code='rpc'}
			elseif($fault -ceq 'synthetic-raw-envelope'){$envelope=$data.response.result.content[0].text | ConvertFrom-Json;$envelope.data.deterministicFallback=$true;$data.response.result.content[0].text=ConvertTo-Json $envelope -Depth 30 -Compress;$code='boolean'}
			else{$data.width=1;$code='decode-join'}
			Write-ProtocolJson $changedPath $data
			if($fault -ceq 'stale-decoder-output'){$row.decoderReceiptPin=Get-ProtocolPin $changedPath}else{$row.rpcPin=Get-ProtocolPin $changedPath}
			Save-ProtocolRaw $r
			Assert-ProtocolReject $code { Test-ProtocolLane $r Editor }
		} finally {[IO.File]::WriteAllBytes($changedPath,$original)}
	}
}
foreach($fault in @('same-target-generation','different-renderer-session','wrong-source-extent','not-dedicated-target','game-view-not-next-frame')) {
	Invoke-ProtocolCheck "Editor-$fault" {
		$r=Copy-ProtocolValue $editorReceipt;$r.watchdogPin=Get-ProtocolPin $watchPath;Sync-ProtocolRaw $r
		$row=$r.observations[0];$rpcPath=$row.rpcPin.path;$original=[IO.File]::ReadAllBytes($rpcPath)
		try {
			$rpc=Get-Content -LiteralPath $rpcPath -Raw | ConvertFrom-Json
			if($fault -ceq 'game-view-not-next-frame'){$rpc.request.params.arguments.freshness='latestCompleted';$code='target'}
			else {
				if($fault -ceq 'same-target-generation'){$row.backendTarget.targetGeneration=$r.warmup.backendTarget.targetGeneration;$code='target'}
				elseif($fault -ceq 'different-renderer-session'){$row.backendTarget.rendererSessionGeneration=2;$code='target'}
				elseif($fault -ceq 'wrong-source-extent'){$row.backendTarget.sourceWidth=1;$code='target'}
				else{$row.backendTarget.dedicatedCaptureTarget=$false;$code='boolean'}
				$envelope=$rpc.response.result.content[0].text | ConvertFrom-Json;$envelope.data.backendTarget=$row.backendTarget
				$rpc.response.result.content[0].text=ConvertTo-Json $envelope -Depth 30 -Compress
			}
			Write-ProtocolJson $rpcPath $rpc;$row.rpcPin=Get-ProtocolPin $rpcPath;Save-ProtocolRaw $r
			Assert-ProtocolReject $code { Test-ProtocolLane $r Editor }
		} finally {[IO.File]::WriteAllBytes($rpcPath,$original)}
	}
}

foreach($lane in @('Native','Editor')) {
	Invoke-ProtocolCheck "$lane-canonical-executable-accepts-generic-path-separators" {
		$r=Copy-ProtocolValue $(if($lane -ceq 'Native'){$native}else{$editorReceipt})
		$r.watchdogPin=Get-ProtocolPin $watchPath
		$r.executablePath=$r.executablePath.Replace('\','/')
		Sync-ProtocolRaw $r
		$result=Test-ProtocolLane $r $lane
		if($result.nativeAcceptance -or !$result.protocolOnly){throw 'Path equivalence promoted protocol evidence.'}
	}
}
Invoke-ProtocolCheck 'Editor-canonical-decoder-accepts-generic-path-separators' {
	$r=Copy-ProtocolValue $editorReceipt;$r.watchdogPin=Get-ProtocolPin $watchPath
	foreach($row in @($r.warmup)+@($r.observations)){$row.decoder.executablePath=$row.decoder.executablePath.Replace('\','/')}
	Sync-ProtocolRaw $r;$result=Test-ProtocolLane $r Editor
	if($result.nativeAcceptance -or !$result.protocolOnly){throw 'Decoder path equivalence promoted protocol evidence.'}
}
Invoke-ProtocolCheck 'canonical-path-normalization-still-rejects-another-installed-file' {
	$r=Copy-ProtocolValue $native;$r.watchdogPin=Get-ProtocolPin $watchPath
	$r.executablePath=$editor.Replace('\','/');Sync-ProtocolRaw $r
	Assert-ProtocolReject 'executable' {Test-ProtocolLane $r Native}
}

# Exercise the actual installed-runner helpers in a child scope. Dot-sourcing
# their public parameter defaults must not replace this test's WorkRoot/counts.
& {
	param([string]$HelperRoot,[string]$RunnerPath)
	. $RunnerPath
	$null=[IO.Directory]::CreateDirectory($HelperRoot)
	$helperInstall=Join-Path $HelperRoot 'mock-install';$helperFiles=@()
	foreach($relative in @(RblRequiredArtifacts)) {
		$path=Join-Path $helperInstall $relative;$null=[IO.Directory]::CreateDirectory((Split-Path $path))
		[IO.File]::WriteAllText($path,'PROTOCOL ONLY - not an executable')
		$pin=RblPin $path;$helperFiles+=,[pscustomobject]@{path=$relative;sha256=$pin.sha256;bytes=$pin.bytes}
	}
	$helperManifest=Join-Path $helperInstall 'Tests/Tools/ReadbackLifetime/readback-lifetime-artifacts.json'
	RblWrite $helperManifest @{schemaVersion='readback-lifetime-artifacts-v1';files=$helperFiles}
	$helperLoaded=RblLoadInstall $helperInstall
	Invoke-ProtocolCheck 'installed-helper-loads-exact-pinned-closure' {
		if($helperLoaded.inventory.Count -ne (RblRequiredArtifacts).Count){throw 'Installed closure lost required files.'}
	}
	Invoke-ProtocolCheck 'installed-helper-trusted-native-counts' {
		$c=RblContext $helperLoaded (Join-Path $HelperRoot 'fresh-work') 'protocol-only' (RblInstalled $helperLoaded 'Tests/Tools/ReadbackLifetime/ReadbackLifetimeFixture.exe') 300 100
		if($c.iterations -ne 300 -or $c.iterationsPerScenario -ne 100 -or $c.protocolContext){throw 'Trusted native context lost its count contract.'}
	}
	Invoke-ProtocolCheck 'installed-helper-fresh-work-and-existing-root-rejection' {
		RblValidateWorkRoot (Join-Path $HelperRoot 'fresh-work') $helperLoaded
		$caught=$false
		try{RblValidateWorkRoot $helperInstall $helperLoaded}catch{if($_.Exception.Message -notlike '*must not exist*'){throw};$caught=$true}
		if(!$caught){throw 'Existing work directory was accepted.'}
	}
	Invoke-ProtocolCheck 'installed-helper-refuses-evidence-overwrite' {
		$caught=$false
		try{RblWrite $helperManifest @{schemaVersion='wrong'}}catch{if($_.Exception.Message -notlike '*must be fresh*'){throw};$caught=$true}
		if(!$caught){throw 'Existing evidence was overwritten.'}
	}
	Invoke-ProtocolCheck 'installed-helper-rejects-installed-byte-drift' {
		$pin=$helperLoaded.pins[0];[IO.File]::AppendAllText($pin.path,'changed')
		$caught=$false
		try{RblCheckPin $pin}catch{if($_.Exception.Message -notlike '*Changed evidence*'){throw};$caught=$true}
		if(!$caught){throw 'Changed installed bytes were accepted.'}
	}

	$nativeFact=[pscustomobject]@{payloadAllocationFaultTriggered=$true;timelineSemaphoreEnabled=$true;gateTimelineSemaphore=$true;gateInitialValue=0;gateWaitValue=1;gateSignalValue=1;gateWaitStageMask=4096;submitAccepted=$true;hostReleased=$true;lastFenceResult=0;terminalDeliveries=1;sourceImageBlocked=$false;sourceBlockerReleases=1;commandPoolDependencyReleases=1;commandPoolAliveAtRetirement=$true;commandPoolDestroyedBeforeRetirement=$false;sourceImageDestroyedBeforeRetirement=$false;validationCallbackInstalled=$true;validationErrorsBefore=0;validationErrorsAfter=0;ownerCreated=[pscustomobject]@{stagingBuffer=1;allocation=1;commandBuffer=1;fence=1};ownerDestroyed=[pscustomobject]@{stagingBuffer=1;allocation=1;commandBuffer=1;fence=1}}
	$fault=[pscustomobject]@{status='Passed';evidenceKind='CppAllocationFaultInjection';nativeFenceObservationsActual=$true;simulatedDeviceLoss=$false;native=$nativeFact;healthyFollowup=[pscustomobject]@{backendReadback=$true;deterministicFallback=$false;checksumsVerified=$true;decodedWidth=2;decodedHeight=3;targetGeneration=2;rendererSessionGeneration=1};contextShutdownCompleted=$true;contextDestructionCompleted=$true;closedNative=[pscustomobject]@{gateDestroyed=1}}
	Invoke-ProtocolCheck 'installed-helper-accepts-labelled-fault-shape-only' {RblCheckFaultQualification $fault}
	Invoke-ProtocolCheck 'installed-helper-rejects-old-event-allocation-fault-gate' {
		$r=Copy-ProtocolValue $fault;$r.native.gateTimelineSemaphore=$false;$caught=$false
		try{RblCheckFaultQualification $r}catch{if($_.Exception.Message -notlike '*actual enabled timeline semaphore gating*'){throw};$caught=$true}
		if(!$caught){throw 'Old event object substituted for the legal timeline gate.'}
	}
	Invoke-ProtocolCheck 'installed-helper-rejects-untriggered-allocation-fault' {
		$r=Copy-ProtocolValue $fault;$r.native.payloadAllocationFaultTriggered=$false;$caught=$false
		try{RblCheckFaultQualification $r}catch{if($_.Exception.Message -notlike '*Injected payload failure*'){throw};$caught=$true}
		if(!$caught){throw 'Untriggered allocation checkpoint was accepted.'}
	}
	Invoke-ProtocolCheck 'installed-helper-rejects-double-native-owner-destruction' {
		$r=Copy-ProtocolValue $fault;$r.native.ownerDestroyed.fence=2;$caught=$false
		try{RblCheckFaultQualification $r}catch{if($_.Exception.Message -notlike '*owner balance failed*'){throw};$caught=$true}
		if(!$caught){throw 'Double fence destruction was accepted.'}
	}

	$marker='[VulkanValidation] layer=VK_LAYER_KHRONOS_validation callbackInstalled=true'
	Invoke-ProtocolCheck 'installed-helper-observes-actual-validation-startup-marker' {
		$observation=RblCheckEditorValidationText ('[2026-10-16][INFO] '+$marker) 'healthy shutdown'
		if(!$observation.actuallyEnabled -or !$observation.callbackInstalled){throw 'Actual initialization marker was lost.'}
	}
	foreach($sample in @(
		[pscustomobject]@{name='requested-prefs-only';stdout='preferences vulkanValidationLayers=true'},
		[pscustomobject]@{name='missing-marker';stdout=''},
		[pscustomobject]@{name='unavailable-layer';stdout=($marker+"`nvalidation layers requested, but not available; continuing without validation layers.")}
	)) {
		Invoke-ProtocolCheck ('installed-helper-validation-incomplete-'+$sample.name) {
			$caught=$false
			try{RblCheckEditorValidationText $sample.stdout '' | Out-Null}catch{if(!$_.Exception.Message.StartsWith('INCOMPLETE:')){throw};$caught=$true}
			if(!$caught){throw 'Missing/unavailable actual validation was accepted.'}
		}
	}
	Invoke-ProtocolCheck 'installed-helper-rejects-actual-validation-error-through-teardown' {
		$caught=$false
		try{RblCheckEditorValidationText $marker '[2026-10-16][ERROR][callback] [Vulkan] teardown failure' | Out-Null}catch{if($_.Exception.Message -notlike '*validation reported an error*'){throw};$caught=$true}
		if(!$caught){throw 'Actual teardown validation error was accepted.'}
	}
} (Join-Path $work 'installed-helper-checks') (Join-Path $PSScriptRoot 'RunInstalledReadbackLifetime.ps1')

$report=[pscustomobject]@{schemaVersion='readback-lifetime-protocol-result-v1';status='ProtocolPassed';caseCount=$script:checks;protocolFixtures=$true;nativeAcceptance=$false;editorAcceptance=$false;realProcessesLaunched=$false}
Write-ProtocolJson (Join-Path $work 'result.json') $report
Write-Output "Readback protocol evidence: $work"
Write-Output "$($script:checks) readback protocol cases passed; synthetic parser evidence only."
