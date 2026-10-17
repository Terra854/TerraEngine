<#
Run the installed readback lifetime campaign. Requires a complete CMake install,
PowerShell 7, a usable Vulkan adapter/desktop and enabled validation layers.
Example: & Build/Tests/Tools/ReadbackLifetime/RunInstalledReadbackLifetime.ps1 `
 -InstallRoot C:/Terra/Build -WorkRoot C:/Terra/out/fresh-readback -Mode All -Iterations 100
Iterations is per native scenario (three scenarios), and the editor capture count.
Exit 0: requested lanes verified; 1: failed; 2: prerequisite incomplete.
Dot-sourcing defines shared installed-runner helpers without launching anything.
#>
[CmdletBinding()]
param([string]$InstallRoot='', [string]$WorkRoot='',
	[ValidateSet('Native','Editor','All')][string]$Mode='All',
	[ValidateRange(100,100000)][int]$Iterations=100)

function RblRequire([bool]$Condition,[string]$Message) { if(!$Condition){throw $Message} }
function RblBelow([string]$Path,[string]$Root) {
	return [IO.Path]::GetFullPath($Path).StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)
}
function RblNoRedirect([string]$Path) {
	$cursor=[IO.Path]::GetFullPath($Path)
	while($cursor){
		if(Test-Path -LiteralPath $cursor){RblRequire (((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Redirected path: $cursor"}
		$cursor=[IO.Path]::GetDirectoryName($cursor)
	}
}
function RblPin([string]$Path) {
	RblRequire ([IO.Path]::IsPathRooted($Path)) 'Evidence paths must be absolute.';RblNoRedirect $Path
	RblRequire ([IO.File]::Exists($Path)) "INCOMPLETE: Missing installed/evidence file: $Path"
	return [pscustomobject]@{path=[IO.Path]::GetFullPath($Path);sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $Path).Length}
}
function RblCheckPin($Pin) {
	$actual=RblPin $Pin.path
	RblRequire ($actual.sha256 -ceq $Pin.sha256 -and $actual.bytes -eq $Pin.bytes) "Changed evidence: $($Pin.path)"
}
function RblWrite([string]$Path,$Value) {
	RblNoRedirect $Path
	RblRequire (!(Test-Path -LiteralPath $Path)) "Evidence output must be fresh: $Path"
	$bytes=[Text.UTF8Encoding]::new($false).GetBytes((ConvertTo-Json -InputObject $Value -Depth 100)+"`n")
	$stream=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
	try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
}
function RblLoadInstall([string]$Root) {
	RblRequire (![string]::IsNullOrWhiteSpace($Root)) 'InstallRoot is required.'
	$rootPath=[IO.Path]::GetFullPath($Root);RblNoRedirect $rootPath
	$manifestPin=RblPin (Join-Path $rootPath 'Tests/Tools/ReadbackLifetime/readback-lifetime-artifacts.json')
	$manifest=Get-Content -LiteralPath $manifestPin.path -Raw | ConvertFrom-Json
	RblRequire ($manifest.schemaVersion -ceq 'readback-lifetime-artifacts-v1') 'Wrong readback installed manifest schema.'
	$inventory=@{};$pins=[Collections.Generic.List[object]]::new()
	foreach($row in @($manifest.files)) {
		RblRequire (![IO.Path]::IsPathRooted($row.path) -and !$inventory.ContainsKey($row.path)) 'Duplicate or absolute artifact path.'
		$path=[IO.Path]::GetFullPath((Join-Path $rootPath $row.path));RblRequire (RblBelow $path $rootPath) 'Artifact escaped install root.'
		$pin=RblPin $path;RblRequire ($pin.sha256 -ceq $row.sha256 -and $pin.bytes -eq $row.bytes) "Installed artifact hash mismatch: $($row.path)"
		$inventory[$row.path]=$path;$pins.Add($pin)
	}
	RblRequire ($inventory.Count -gt 0) 'Empty installed artifact closure.'
	return [pscustomobject]@{root=$rootPath;manifestPin=$manifestPin;inventory=$inventory;pins=$pins.ToArray()}
}
function RblInstalled($Install,[string]$Relative) {
	RblRequire ($Install.inventory.ContainsKey($Relative)) "INCOMPLETE: Missing required installed artifact: $Relative"
	return $Install.inventory[$Relative]
}
function RblRequiredArtifacts() {
	return @('Tests/Tools/ReadbackLifetime/ReadbackLifetimeFixture.exe',
		'Tests/Tools/ReadbackLifetime/RunInstalledReadbackLifetime.ps1',
		'Tests/Tools/ReadbackLifetime/InvokeReadbackEditorAcceptance.ps1',
		'Tests/Tools/ReadbackLifetime/ReadbackLifetimeProtocol.psm1',
		'Tests/Tools/Correctness/RunWithWatchdog.ps1','Editor/TerraEngine-Editor.exe',
		'Editor/TerraEngine.dll','Editor/glfw3.dll',
		'Tests/Tools/RuntimeMemorySafety/MaterializeRuntimeMemorySafetyFixture.ps1',
		'Tests/Tools/RuntimeMemorySafety/Fixtures/manifest.json',
		'Tests/Tools/RendererEfficiency/TerraEngine-RendererEfficiencyFixtureMaterializer.exe',
		'Tests/Fixtures/RendererEfficiency/workloads.json')
}
function RblValidateWorkRoot([string]$Work,$Install) {
	RblRequire (![string]::IsNullOrWhiteSpace($Work)) 'WorkRoot is required.'
	RblNoRedirect $Work
	RblRequire (!(Test-Path -LiteralPath $Work)) 'WorkRoot must not exist.'
	RblRequire ([IO.Path]::GetFullPath($Work) -ine [IO.Path]::GetFullPath($Install.root) -and !(RblBelow $Work $Install.root) -and !(RblBelow $Install.root $Work)) 'WorkRoot overlaps install.'
}
function RblContext($Install,[string]$Work,[string]$Nonce,[string]$Executable,[int]$Count,[int]$PerScenario=100) {
	return [pscustomobject]@{protocolContext=$false;runNonce=$Nonce;installRoot=$Install.root;workRoot=$Work;
		manifestPin=$Install.manifestPin;requiredArtifacts=@(RblRequiredArtifacts);iterations=$Count;iterationsPerScenario=$PerScenario;
		executablePath=$Executable;decoderExecutablePath=(RblInstalled $Install 'Tests/Tools/ReadbackLifetime/ReadbackLifetimeFixture.exe')}
}
function RblCheckWatchdog([string]$Directory,[int]$ExitCode) {
	$path=Join-Path $Directory 'watchdog.json';$watch=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
	RblRequire ($ExitCode -eq 0 -and $watch.exitCode -is [ValueType] -and $watch.exitCode -isnot [bool] -and $watch.exitCode -eq 0 -and $watch.timedOut -is [bool] -and !$watch.timedOut) 'Installed process failed or watchdog terminated it.'
	return $watch
}
function RblCheckEditorValidationText([string]$RawStdout,[string]$RawStderr) {
	# This marker is emitted only after the actual enabled layer and successful
	# debug-messenger creation. Preferences are a request, never this observation.
	$marker='[VulkanValidation] layer=VK_LAYER_KHRONOS_validation callbackInstalled=true'
	$combined=$RawStdout+"`n"+$RawStderr
	RblRequire (!($combined -match '(?m)^.*\[ERROR\].*\[Vulkan\]')) 'Editor Vulkan validation reported an error; raw logs include teardown.'
	RblRequire (!($combined -match 'validation layers requested, but not available')) 'INCOMPLETE: Editor explicitly reported unavailable Vulkan validation layers.'
	RblRequire ($RawStdout.Contains($marker)) 'INCOMPLETE: Owned editor logs lack the actual initialized Vulkan validation/debug-callback observation.'
	return [pscustomobject]@{actuallyEnabled=$true;callbackInstalled=$true;enabledLayer='VK_LAYER_KHRONOS_validation';startupMarker=$marker;capturedThroughNormalClose=$true}
}
function RblCheckFaultQualification($Fault) {
	RblRequire ($Fault.status -ceq 'Passed' -and $Fault.evidenceKind -ceq 'CppAllocationFaultInjection' -and $Fault.nativeFenceObservationsActual -eq $true -and $Fault.simulatedDeviceLoss -eq $false) 'Missing actual typed C++ allocation fault qualification.'
	$n=$Fault.native
	RblRequire ($n.timelineSemaphoreEnabled -eq $true -and $n.gateTimelineSemaphore -eq $true -and $n.gateInitialValue -eq 0 -and $n.gateWaitValue -eq 1 -and $n.gateSignalValue -eq 1 -and $n.gateWaitStageMask -eq 4096) 'Allocation fault did not use actual enabled timeline semaphore gating.'
	RblRequire ($n.payloadAllocationFaultTriggered -eq $true -and $n.submitAccepted -eq $true -and $n.hostReleased -eq $true -and $n.lastFenceResult -eq 0 -and $n.terminalDeliveries -eq 1 -and $n.sourceImageBlocked -eq $false -and $n.sourceBlockerReleases -eq 1 -and $n.commandPoolDependencyReleases -eq 1) 'Injected payload failure did not retain, complete and settle real native ownership once.'
	RblRequire ($n.commandPoolAliveAtRetirement -eq $true -and !$n.commandPoolDestroyedBeforeRetirement -and !$n.sourceImageDestroyedBeforeRetirement -and $n.validationCallbackInstalled -eq $true -and $n.validationErrorsBefore -eq 0 -and $n.validationErrorsAfter -eq 0) 'Allocation fault crossed an invalid native dependency or validation boundary.'
	foreach($kind in @('stagingBuffer','allocation','commandBuffer','fence')){RblRequire ($n.ownerCreated.$kind -eq 1 -and $n.ownerDestroyed.$kind -eq 1) "Allocation fault owner balance failed: $kind"}
	$healthy=$Fault.healthyFollowup
	RblRequire ($healthy.backendReadback -eq $true -and $healthy.deterministicFallback -eq $false -and $healthy.checksumsVerified -eq $true -and $healthy.decodedWidth -gt 0 -and $healthy.decodedHeight -gt 0 -and $healthy.targetGeneration -gt 0 -and $healthy.rendererSessionGeneration -gt 0) 'Allocation fault blocked the independently decoded healthy retry.'
	RblRequire ($Fault.contextShutdownCompleted -eq $true -and $Fault.contextDestructionCompleted -eq $true -and $Fault.closedNative.gateDestroyed -eq 1) 'Allocation fault timeline semaphore/context teardown did not complete.'
}
function RblRunNative($Install,[string]$Work,[int]$PerScenario) {
	$null=[IO.Directory]::CreateDirectory($Work)
	$fixture=RblInstalled $Install 'Tests/Tools/ReadbackLifetime/ReadbackLifetimeFixture.exe'
	$watchdog=RblInstalled $Install 'Tests/Tools/Correctness/RunWithWatchdog.ps1'
	$nonce=[guid]::NewGuid().ToString('N');$rawPath=Join-Path $Work 'native-output.json';$logs=Join-Path $Work 'process'
	& $watchdog -Executable $fixture -ArgumentList @('--mode','native','--install-root',$Install.root,'--output',$rawPath,'--run-nonce',$nonce,'--iterations',[string](3*$PerScenario)) -WorkingDirectory (Split-Path $fixture) -TimeoutSeconds 3600 -LogDirectory $logs | Out-Host
	$exitCode=$LASTEXITCODE
	RblRequire (Test-Path -LiteralPath $rawPath) 'Native fixture produced no raw output.'
	$raw=Get-Content -LiteralPath $rawPath -Raw | ConvertFrom-Json
	if($exitCode -eq 2 -and $raw.status -ceq 'Incomplete'){throw "INCOMPLETE: $($raw.reason)"}
	$watch=RblCheckWatchdog $logs $exitCode
	RblRequire ($raw.status -ceq 'Passed' -and $raw.processId -eq $watch.pid -and [IO.Path]::GetFullPath($raw.executablePath) -ieq [IO.Path]::GetFullPath($fixture) -and $raw.runNonce -ceq $nonce) 'Native output identity/status does not match owned launch.'
	RblCheckFaultQualification $raw.faultQualification
	# Keep the complete native output, including allocation-fault and callback
	# evidence; append only runner-owned provenance after the child has exited.
	$receipt=$raw | ConvertTo-Json -Depth 100 | ConvertFrom-Json
	$receipt.schemaVersion='readback-lifetime-native-v1'
	$receipt | Add-Member -NotePropertyMembers @{installManifestSha256=$Install.manifestPin.sha256;watchdogPin=(RblPin (Join-Path $logs 'watchdog.json'));rawObservationsPin=(RblPin $rawPath);stdoutPin=(RblPin (Join-Path $logs 'stdout.log'));stderrPin=(RblPin (Join-Path $logs 'stderr.log'))}
	$context=RblContext $Install $Work $nonce $fixture (3*$PerScenario) $PerScenario
	$verification=Test-ReadbackNativeReceipt $receipt $context
	RblWrite (Join-Path $Work 'receipt.json') $receipt
	RblWrite (Join-Path $Work 'verification.json') $verification
	return [pscustomobject]@{lane='Native';status='Passed';receiptPin=(RblPin (Join-Path $Work 'receipt.json'));verificationPin=(RblPin (Join-Path $Work 'verification.json'));rawPin=(RblPin $rawPath)}
}

if($MyInvocation.InvocationName -eq '.'){return}
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$resultPath=$null;$failure=$null;$exitCode=1;$lanes=[Collections.Generic.List[object]]::new()
try {
	$install=RblLoadInstall $InstallRoot
	foreach($required in @(RblRequiredArtifacts)){$null=RblInstalled $install $required}
	RblRequire ([IO.Path]::GetFullPath($PSCommandPath) -ieq [IO.Path]::GetFullPath((RblInstalled $install 'Tests/Tools/ReadbackLifetime/RunInstalledReadbackLifetime.ps1'))) 'Invoke the installed runner, not a source copy.'
	$work=[IO.Path]::GetFullPath($WorkRoot);RblValidateWorkRoot $work $install
	$null=[IO.Directory]::CreateDirectory($work);$resultPath=Join-Path $work 'result.json'
	Import-Module (RblInstalled $install 'Tests/Tools/ReadbackLifetime/ReadbackLifetimeProtocol.psm1') -Force
	if($Mode -in @('Native','All')){$lanes.Add((RblRunNative $install (Join-Path $work 'native') $Iterations))}
	if($Mode -in @('Editor','All')) {
		$helper=RblInstalled $install 'Tests/Tools/ReadbackLifetime/InvokeReadbackEditorAcceptance.ps1';$output=Join-Path $work 'editor-receipt.json'
		& $helper -InstallRoot $install.root -WorkRoot (Join-Path $work 'editor') -OutputPath $output -Iterations $Iterations | Out-Host
		$childExit=$LASTEXITCODE
		if($childExit -eq 2){throw 'INCOMPLETE: Installed editor prerequisites or provenance are unavailable; inspect retained editor evidence.'}
		RblRequire ($childExit -eq 0 -and (Test-Path -LiteralPath $output)) 'Installed editor loopback failed.'
		$receipt=Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
		RblCheckPin $receipt.stdoutPin;RblCheckPin $receipt.stderrPin
		$observedValidation=RblCheckEditorValidationText ([IO.File]::ReadAllText($receipt.stdoutPin.path)) ([IO.File]::ReadAllText($receipt.stderrPin.path))
		RblRequire ($receipt.validationObservation.startupMarker -ceq $observedValidation.startupMarker -and $receipt.validationObservation.actuallyEnabled -eq $true -and $receipt.validationObservation.callbackInstalled -eq $true) 'Editor validation receipt differs from its pinned actual startup/shutdown logs.'
		$context=RblContext $install (Join-Path $work 'editor') $receipt.runNonce (RblInstalled $install 'Editor/TerraEngine-Editor.exe') $Iterations
		$verification=Test-ReadbackEditorReceipt $receipt $context
		$lanes.Add([pscustomobject]@{lane='Editor';status='Passed';receiptPin=(RblPin $output);verification=$verification})
	}
	foreach($pin in @($install.manifestPin)+@($install.pins)){RblCheckPin $pin}
	$exitCode=0
} catch {$failure=$_.Exception.Message;$exitCode=if($failure.StartsWith('INCOMPLETE:')){2}else{1}}
if($null -ne $resultPath){RblWrite $resultPath ([pscustomobject]@{schemaVersion='readback-lifetime-campaign-v1';status=$(if($exitCode -eq 0){'Passed'}elseif($exitCode -eq 2){'Incomplete'}else{'Failed'});reason=$failure;mode=$Mode;iterationsPerScenario=$Iterations;lanes=$lanes.ToArray();nativeAcceptance=($exitCode -eq 0 -and $Mode -in @('Native','All'));editorAcceptance=($exitCode -eq 0 -and $Mode -in @('Editor','All'))})}
Write-Output "Installed readback lifetime campaign exit $exitCode; evidence: $resultPath"
if($failure){Write-Output $failure}
exit $exitCode
