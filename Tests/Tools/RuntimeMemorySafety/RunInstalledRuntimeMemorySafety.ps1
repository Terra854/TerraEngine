<#
Installed runtime memory safety validation with strict evidence checks.
Invoke the installed copy with a fresh WorkRoot. Frozen timing tools have their
own ToolInstallRoot; candidate runtime/native fixtures always use InstallRoot.
Exit0 means all requested lanes passed,1 failed,2 unavailable/unverified.
#>
[CmdletBinding()]
param(
	[string]$InstallRoot='', [string]$WorkRoot='',
	[ValidateSet('Cpu','Vulkan','Renderer','Performance','All')][string]$Mode='Cpu',
	[ValidateRange(1,10000)][int]$Iterations=100, [int[]]$WorkerCounts=@(1,2,8),
	[string]$ReferenceInstallRoot='', [string]$ReferenceCalibrationPath='',
	[string]$ToolInstallRoot='', [string]$ParityEvidencePath='',
	[string]$EditorDiagnosticsEvidencePath='',
	[switch]$VulkanHostInstance,
	[ValidateRange(1024,65535)][int]$McpPort=43186
)
function RmsRequire([bool]$Condition,[string]$Message){if(!$Condition){throw $Message}}
function RmsField($Value,[string]$Name){
	if($Value -is [Collections.IDictionary] -and $Value.Contains($Name)){return $Value[$Name]}
	if($null -eq $Value -or $null -eq $Value.PSObject.Properties[$Name]){throw "Missing evidence field: $Name"};return $Value.$Name
}
function RmsNoRedirect([string]$Path){
	$cursor=[IO.Path]::GetFullPath($Path)
	while($cursor){try{RmsRequire (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Redirected path: $cursor"}catch [IO.FileNotFoundException]{}catch [IO.DirectoryNotFoundException]{};$cursor=[IO.Path]::GetDirectoryName($cursor)}
}
function RmsBelow([string]$Child,[string]$Root){return [IO.Path]::GetFullPath($Child).StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}
function RmsPin([string]$Path){
	RmsRequire ([IO.Path]::IsPathRooted($Path)) 'Evidence paths must be absolute';RmsNoRedirect $Path
	if(![IO.File]::Exists($Path)){throw "UNVERIFIED: Missing evidence: $Path"}
	return [pscustomobject]@{path=[IO.Path]::GetFullPath($Path);sha256=(Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $Path).Length}
}
function RmsCheckPin($Pin){$actual=RmsPin (RmsField $Pin 'path');RmsRequire ($actual.sha256 -ceq ([string](RmsField $Pin 'sha256')).ToLowerInvariant() -and $actual.bytes -eq (RmsField $Pin 'bytes')) "Changed evidence: $($actual.path)"}
function RmsWrite([string]$Path,$Value){
	RmsNoRedirect $Path;RmsRequire (!(Test-Path -LiteralPath $Path)) 'Evidence output must be fresh'
	$bytes=[Text.UTF8Encoding]::new($false).GetBytes((ConvertTo-Json -InputObject $Value -Depth 100));$stream=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
	try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
}
function RmsLoadInstall([string]$Root){
	$rootPath=[IO.Path]::GetFullPath($Root);RmsNoRedirect $rootPath
	$manifest=Join-Path $rootPath 'Tests/Tools/RuntimeMemorySafety/runtime-memory-safety-artifacts.json';$pin=RmsPin $manifest
	$data=Get-Content -LiteralPath $manifest -Raw|ConvertFrom-Json
	RmsRequire ((RmsField $data 'schemaVersion') -ceq 'runtime-memory-safety-v1') 'Wrong installed manifest schema'
	$inventory=@{};$pins=New-Object 'Collections.Generic.List[object]'
	foreach($row in @(RmsField $data 'files')){
		$relative=RmsField $row 'path';RmsRequire (![IO.Path]::IsPathRooted($relative) -and !$inventory.ContainsKey($relative)) 'Invalid or duplicate manifest path'
		$path=[IO.Path]::GetFullPath((Join-Path $rootPath $relative));RmsRequire (RmsBelow $path $rootPath) 'Manifest path escapes install'
		$actual=RmsPin $path;RmsRequire ($actual.bytes -eq (RmsField $row 'length') -and $actual.sha256 -ceq ([string](RmsField $row 'sha256')).ToLowerInvariant()) "Installed hash mismatch: $relative"
		$inventory[$relative]=$path;$pins.Add($actual)
	}
	return [pscustomobject]@{root=$rootPath;manifest=$pin;inventory=$inventory;pins=$pins.ToArray()}
}
function RmsInstalled($Install,[string]$Relative){RmsRequire ($Install.inventory.ContainsKey($Relative)) "Missing manifest artifact: $Relative";return $Install.inventory[$Relative]}
function RmsAssertInvocationPaths($Install,[string]$Work,[string]$InvokedPath,[string[]]$OtherRoots=@()){
	RmsNoRedirect $Work;RmsRequire (!(Test-Path -LiteralPath $Work)) 'WorkRoot must be fresh'
	foreach($root in @($Install.root)+$OtherRoots|Where-Object {$_}){RmsRequire ($Work -ine $root -and !(RmsBelow $Work $root) -and !(RmsBelow $root $Work)) 'WorkRoot overlaps an install'}
	RmsRequire ([IO.Path]::GetFullPath($InvokedPath) -ieq (RmsInstalled $Install 'Tests/Tools/RuntimeMemorySafety/RunInstalledRuntimeMemorySafety.ps1')) 'Invoke installed runner, not source'
}
function RmsValidateRegistry($Registry){
	$floors=@{'Engine.RuntimeMemorySafety.TaskCache'=8;'Engine.RuntimeMemorySafety.DependencyAdmission'=10;'Engine.RuntimeMemorySafety.Allocators.FixedPool'=4;'Engine.RuntimeMemorySafety.Allocators.Alignment'=4;'Engine.RuntimeMemorySafety.Allocators.Overflow'=6;'Engine.RuntimeMemorySafety.RendererScratch'=10;'Engine.RuntimeMemorySafety.EmptyFramePolicy'=8;'Editor.RuntimeMemorySafety.Diagnostics'=4}
	RmsRequire ($Registry.minimumUniqueCases -ge 54 -and $Registry.minimumEngineCases -ge 50 -and $Registry.minimumEditorCases -ge 4) 'Frozen registry floors were weakened'
	$unique=@{};$prefixes=@{}
	foreach($group in $Registry.groups){
		RmsRequire (!$prefixes.ContainsKey($group.prefix)) 'Duplicate native registry prefix';$prefixes[$group.prefix]=$true
		if($floors.ContainsKey($group.prefix)){RmsRequire ($group.minimumRegisteredCases -ge $floors[$group.prefix] -and @($group.cases).Count -ge $floors[$group.prefix]) 'Required group floor was weakened'}
		foreach($name in $group.cases){RmsRequire ($name.StartsWith($group.prefix+'.',[StringComparison]::Ordinal) -and !$unique.ContainsKey($name)) 'Duplicate or misgrouped required case';$unique[$name]=$true}
	}
	foreach($prefix in $floors.Keys){RmsRequire ($prefixes.ContainsKey($prefix)) "Missing frozen native group: $prefix"}
	RmsRequire ($unique.Count -ge 54) 'Registry does not contain54 unique cases'
}
function RmsObservationIdentity($Value){
	$ids=@();foreach($key in @('caseId','case')){if($null -ne $Value.PSObject.Properties[$key]){$ids+=,[string]$Value.$key}}
	RmsRequire ($ids.Count -ge 1 -and ![string]::IsNullOrWhiteSpace($ids[0])) 'Native observation has no case identity'
	RmsRequire (@($ids|Where-Object {$_ -cne $ids[0]}).Count -eq 0) 'Conflicting native observation identities';return $ids[0]
}
function RmsParseNative([string]$Stdout,$Watchdog,[int]$ExitCode,[string[]]$Expected,[int]$MinimumCases,[int]$ExpectedWorkers=0){
	RmsRequire ($ExitCode -eq 0 -and (RmsField $Watchdog 'exitCode') -eq 0 -and (RmsField $Watchdog 'timedOut') -eq $false) 'Native process failed or watchdog killed it'
	# Preserve failure evidence before any parsing-copy normalization. Raw files
	# and their pins remain untouched by this function.
	RmsRequire (![regex]::IsMatch($Stdout,'\[(FAIL|ERROR)\]')) 'Native failure appears in raw output'
	$clean=[regex]::Replace($Stdout,'\x1B\[[0-?]*[ -/]*[@-~]','')
	RmsRequire (![regex]::IsMatch($clean,'\[(FAIL|ERROR)\]')) 'Native failure appears in raw output'
	# Logger emits each complete structured record atomically, but harness PASS
	# markers/names and observation prefixes/JSON are separate insertions. Remove
	# only these two exact healthy worker messages from the parsing copy so an
	# interleaved record cannot become part of a case name or block its JSON.
	$healthyWorkerInfo='\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\]\[INFO\]\[ThreadPool::workerThread(?:::<lambda_[0-9]+>::operator)?:[0-9]+\] Worker thread no\. [0-9]+ (?:initialized and running|shutting down)\.'
	$clean=[regex]::Replace($clean,$healthyWorkerInfo,'')
	# The asynchronous logger can split the harness's count and phrase across
	# lines. Accept whitespace there while preserving the numeric boundary,
	# unique summary, exact selected names and unchanged registration floor.
	$summary=[regex]::Matches($clean,'(?m)(?<![0-9A-Za-z])(\d+)\s+selected tests\b[^\r\n]*')
	RmsRequire ($summary.Count -eq 1 -and [int]$summary[0].Groups[1].Value -ge $MinimumCases) 'Native selection missing or below frozen floor'
	$names=@([regex]::Matches($clean,'(?m)\[PASS\]\s+([^\r\n]+)')|ForEach-Object {$_.Groups[1].Value.Trim()})
	RmsRequire ($names.Count -eq [int]$summary[0].Groups[1].Value -and @($names|Select-Object -Unique).Count -eq $names.Count) 'PASS names do not match selected count'
	RmsRequire ($names.Count -eq $Expected.Count) 'Substring native filter selected unexpected cases'
	foreach($name in $Expected){RmsRequire ($names -ccontains $name) "Missing exact required native case: $name"}
	$observations=New-Object 'Collections.Generic.List[object]'
	foreach($match in [regex]::Matches($clean,'RMS_OBSERVATION\s+(\{[^\r\n]*\})')){
		$raw=$match.Groups[1].Value;$value=$raw|ConvertFrom-Json;$case=RmsObservationIdentity $value
		RmsRequire ($names -ccontains $case) 'Observation belongs to an unselected case'
		$observations.Add([pscustomobject]@{caseId=$case;facts=$value;rawJson=$raw})
	}
	foreach($name in $Expected){
		$requiresFacts=$name -match '^Engine\.RuntimeMemorySafety\.(TaskCache|DependencyAdmission|Allocators|RendererScratch)\.'
		$facts=@($observations|Where-Object {$_.caseId -ceq $name})
		if($requiresFacts){RmsRequire ($facts.Count -ge 1) "UNVERIFIED: Missing actual native facts: $name"}
		if($name -match '^Engine\.RuntimeMemorySafety\.TaskCache\.'){
			foreach($row in $facts){$v=$row.facts;RmsRequire ((RmsField $v 'drainComplete') -eq $true -and (RmsField $v 'recyclingOpen') -eq $false -and (RmsField $v 'outstandingOwners') -eq 0 -and (RmsField $v 'cachedTasks') -eq 0 -and (RmsField $v 'destroyedCaptures') -eq (RmsField $v 'expectedDestroyedCaptures') -and (RmsField $v 'actualWorkers') -eq 1) 'Native cache ownership did not balance'}
		}
		if($name -match '^Engine\.RuntimeMemorySafety\.DependencyAdmission\.'){
			$workers=$ExpectedWorkers
			if($name.EndsWith('.OneWorkerHeldSibling')){$workers=1}elseif($name.EndsWith('.TwoWorkerHeldSibling')){$workers=2}elseif($name.EndsWith('.EightWorkerHeldSibling')){$workers=8}
			foreach($row in $facts){
				$v=$row.facts;if($workers -gt 0){RmsRequire ((RmsField $v 'actualWorkers') -eq $workers) 'Admission worker count was requested but not observed'}
				foreach($key in @('originalExceptionPreserved','originalErrorTypePreserved','originalErrorMessagePreserved','originalErrorTokenPreserved','heldAncestorsPending','independentWorkCompleted','drainComplete','inputAliveWhileHeld','inputAliveAtDrain','inputSentinelsValid','graphOwnershipBalanced','inputExpiredAfterRetirement')){RmsRequire ((RmsField $v $key) -eq $true) "Admission failed actual settlement condition: $key"}
				RmsRequire ((RmsField $v 'failureCheckpointCount') -eq 1 -and (RmsField $v 'terminalAncestors') -eq (RmsField $v 'expectedTerminalAncestors') -and (RmsField $v 'settledSiblingCount') -eq (RmsField $v 'heldSiblingCount') -and (RmsField $v 'failedParentBodyCalls') -eq 0 -and (RmsField $v 'ancestorBodyCalls') -eq 0) 'Admission failure did not preserve and settle the actual graph'
				RmsRequire ((RmsField $v 'inputReadsBeforeGates') -eq (RmsField $v 'heldSiblingCount') -and (RmsField $v 'inputReadsAfterGates') -eq (RmsField $v 'heldSiblingCount') -and (RmsField $v 'inputDestructionCount') -eq 1) 'Admission input lifetime did not survive held workers and retire once'
			}
		}
	}
	return [pscustomobject]@{selectedCases=$names.Count;passNames=$names;observations=$observations.ToArray()}
}
function RmsRunNative($Context,[string[]]$Cases,[int]$MinimumCases,[string]$Label,[int]$Workers=0){
	$logs=Join-Path $Context.work $Label;$filter=$Cases -join ',';$binary=$(if($Cases[0].StartsWith('Editor.')){$Context.editorTests}else{$Context.engineTests})
	$previous=[Environment]::GetEnvironmentVariable('TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS')
	try{
		# Default native groups must see an absent override, regardless of the
		# caller's environment. Explicit matrix groups receive their exact count.
		if($Workers -gt 0){[Environment]::SetEnvironmentVariable('TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS',[string]$Workers)}
		else{Remove-Item -LiteralPath 'Env:TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS' -ErrorAction SilentlyContinue}
		& $Context.slice -Executable $binary -Filter $filter -MinimumCases $MinimumCases -TimeoutSeconds 10 -LogDirectory $logs|Out-Host;$code=$LASTEXITCODE
	}finally{
		# .NET9 can turn a null assignment into an existing empty variable. Native
		# parsing distinguishes those states, so remove only a formerly absent
		# variable and restore an existing value (including empty) exactly.
		if($null -eq $previous){Remove-Item -LiteralPath 'Env:TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS' -ErrorAction SilentlyContinue}
		else{[Environment]::SetEnvironmentVariable('TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS',$previous)}
	}
	$watchdogPath=Join-Path $logs 'watchdog.json';$stdoutPath=Join-Path $logs 'stdout.log';$stderrPath=Join-Path $logs 'stderr.log'
	$watch=Get-Content -LiteralPath $watchdogPath -Raw|ConvertFrom-Json
	$rawPins=@((RmsPin $watchdogPath),(RmsPin $stdoutPath),(RmsPin $stderrPath))
	try{$result=RmsParseNative ([IO.File]::ReadAllText($stdoutPath)) $watch $code $Cases $MinimumCases $Workers}
	catch{RmsWrite (Join-Path $logs 'native-validation.json') @{scenarioId=$Label;status='Failed';reason=$_.Exception.Message;childExitCode=$code;watchdog=$watch;rawEvidence=$rawPins};throw "Native validation failed: $Label; retained evidence: $logs; $($_.Exception.Message)"}
	$receipt=[pscustomobject]@{scenarioId=$Label;status='Passed';workerCount=$Workers;selectedCases=$result.selectedCases;passNames=$result.passNames;nativeObservations=$result.observations;watchdog=$watch;rawEvidence=$rawPins}
	RmsWrite (Join-Path $logs 'native-validation.json') $receipt;return $receipt
}
function RmsRunCpu($Context,[int]$Iterations,[int[]]$WorkerCounts){
	RmsRequire ($Iterations -ge 100 -and (($WorkerCounts|Sort-Object)-join ',') -ceq '1,2,8') 'Acceptance requires100 repetitions and exact worker matrix1,2,8'
	$rows=New-Object 'Collections.Generic.List[object]'
	foreach($group in $Context.registry.groups){$rows.Add((RmsRunNative $Context @($group.cases) $group.minimumRegisteredCases ('native-'+$group.prefix)))}
	foreach($name in @('CompletedNestedCapture','NeverDispatchedNestedCapture')){for($i=1;$i -le $Iterations;$i++){$rows.Add((RmsRunNative $Context @("Engine.RuntimeMemorySafety.TaskCache.$name") 1 "cache-$name-$i"))}}
	$admission=@($Context.registry.groups|Where-Object {$_.prefix -ceq 'Engine.RuntimeMemorySafety.DependencyAdmission'})[0]
	# The initial group proves all ten registered fault scenarios. Repetitions
	# exercise the multi-ancestor graph once per worker/iteration: 300 faults at
	# the required minimum, without multiplying seven scenarios into 2100 runs.
	$repeatedCase='Engine.RuntimeMemorySafety.DependencyAdmission.MultipleAncestors'
	RmsRequire (@($admission.cases) -ccontains $repeatedCase) 'Frozen admission repetition case is missing'
	foreach($workers in $WorkerCounts){for($i=1;$i -le $Iterations;$i++){$rows.Add((RmsRunNative $Context @($repeatedCase) 1 "admission-$workers-$i" $workers))}}
	return $rows.ToArray()
}
function RmsUint($Value,[string]$Name,[uint64]$Minimum=0,[uint64]$Maximum=9007199254740991){
	$n=RmsField $Value $Name
	RmsRequire ($n -is [ValueType] -and $n -isnot [bool]) "Non-numeric native integer: $Name"
	$d=[double]$n;RmsRequire (![double]::IsNaN($d) -and ![double]::IsInfinity($d) -and $d -eq [Math]::Truncate($d) -and $d -ge $Minimum -and $d -le $Maximum) "Invalid native integer/range: $Name"
	return [uint64]$n
}
function RmsCheckVkResults($Operation){
	foreach($pair in @(@('resetAttempted','resetResult'),@('submitAttempted','submitResult'),@('presentAttempted','presentResult'),@('waitIdleAttempted','waitIdleResult'))){
		$called=RmsField $Operation $pair[0];$result=RmsField $Operation $pair[1]
		RmsRequire ($called -is [bool]) "Native call-attempt field is not boolean: $($pair[0])"
		if($called){RmsRequire ($null -ne $result -and $result -is [ValueType] -and $result -isnot [bool] -and [double]$result -eq [Math]::Truncate([double]$result)) "Called Vulkan operation lacks a real VkResult: $($pair[1])"}
		else{RmsRequire ($null -eq $result) "Uncalled Vulkan operation fabricated a VkResult: $($pair[1])"}
	}
}
function RmsCheckVkIndices($Operation,[bool]$RequireBounds=$true){
	$null=RmsUint $Operation generation 1;$null=RmsUint $Operation sequence 1
	# Current receipts expose the actual native-selected indices but not bundle
	# counts. Reject sentinel/invalid representations; do not invent a count from
	# observed maxima. Actual <count validation is added only when owners expose
	# read-only counts sampled from the active native bundle/frame arrays.
	$image=RmsUint $Operation imageIndex 0 4294967294;$slot=RmsUint $Operation frameSlot 0 4294967294
	if($RequireBounds){RmsRequire ($null -ne $Operation.PSObject.Properties['swapchainImageCount'] -and $null -ne $Operation.PSObject.Properties['frameSlotCount']) 'UNVERIFIED: Actual acquired image/frame counts are missing'}
	if($null -ne $Operation.PSObject.Properties['swapchainImageCount']){$count=RmsUint $Operation swapchainImageCount 1 4294967295;RmsRequire ($image -lt $count) 'Acquired image index exceeds actual bundle count'}
	if($null -ne $Operation.PSObject.Properties['frameSlotCount']){$count=RmsUint $Operation frameSlotCount 1 4294967295;RmsRequire ($slot -lt $count) 'Frame slot exceeds actual native frame count'}
}
function RmsCheckHealthyFrames($Frames,$Operation,[uint64]$RendererSession,[bool]$RequireBounds=$true){
	$frames=@($Frames);RmsRequire ($frames.Count -eq 2) 'Exactly two actual healthy follow-up frames are required'
	$lastFrame=0UL;$lastSerial=0UL;$lastSequence=[uint64]$Operation.sequence;$generation=$null;$backend=$null
	foreach($frame in $frames){
		$id=RmsUint $frame publicationFrameId 1;$session=RmsUint $frame rendererSessionGeneration 1;$resource=RmsUint $frame backendResourceGeneration 1;$native=RmsField $frame 'native'
		RmsCheckVkIndices $native $RequireBounds;RmsCheckVkResults $native
		$serial=RmsUint $native ordinarySubmissionSerial 1
		RmsRequire ($id -gt $lastFrame -and $serial -gt $lastSerial -and $native.sequence -gt $lastSequence -and $native.generation -ge $Operation.generation -and $session -eq $RendererSession) 'Healthy frame/serial/session/generation is stale or unordered'
		if($null -ne $generation){RmsRequire ($native.generation -eq $generation -and $resource -eq $backend) 'Healthy follow-ups disagree on native/resource generation'}
		$generation=$native.generation;$backend=$resource;$lastFrame=$id;$lastSerial=$serial;$lastSequence=$native.sequence
		foreach($key in @('publicationAdmitted','sceneCommandBufferSubmitted')){RmsRequire ((RmsField $frame $key) -eq $true) "Healthy frame missing actual publication: $key"}
		# submitAccepted and recordEnded describe the fallback-only path. Ordinary
		# scene proof uses its own actual submit/serial/completion observations.
		foreach($key in @('acquired','ordinarySubmitAccepted','ordinaryCompletionObserved','submitAttempted','fenceResetAccepted','presentAttempted','waitIdleAttempted','waitIdleCompletionObserved')){RmsRequire ((RmsField $native $key) -eq $true) "Healthy frame missing native proof: $key"}
		RmsRequire ((RmsField $native 'forcedEmpty') -eq $false -and (RmsField $native 'commandBufferCount') -gt 0 -and (RmsField $native 'submitResult') -eq 0 -and (RmsField $native 'resetResult') -eq 0 -and (RmsField $native 'presentResult') -in @(0,1000001003) -and (RmsField $native 'waitIdleResult') -eq 0 -and (RmsField $native 'validationErrorsBefore') -eq 0 -and (RmsField $native 'validationErrorsAfter') -eq 0) 'Healthy ordinary scene operation failed'
	}
}
# A window is retained for the host session while each iteration constructs and
# destroys a separate VulkanContext. Driver handle reuse is valid after teardown;
# the fixture scope ordinal identifies a lifetime, not an invented GPU generation.
function RmsVulkanFlag($Value,[string]$Name,[bool]$Expected=$true){$flag=RmsField $Value $Name;RmsRequire ($flag -is [bool] -and $flag -eq $Expected) "Vulkan lifecycle observation missing/invalid: $Name"}
function RmsVulkanHandle($Value,[string]$Name){
	$handle=RmsField $Value $Name;RmsRequire ($handle -is [string] -and $handle -cmatch '^[0-9]+$' -and $handle -notmatch '^0+$') "Vulkan actual nonnull handle missing: $Name";return $handle
}
function RmsCheckHostInstance($Receipt){
	$hostFacts=RmsField $Receipt hostInstance
	foreach($name in @('requested','handleCleared')){RmsVulkanFlag $hostFacts $name}
	foreach($name in @('hostValidationCallbacks','engineOwnersShared','driverModulePinned','globalProcessInstanceCountClaim','perIterationLastOwnedInstanceTeardownExercised')){RmsVulkanFlag $hostFacts $name $false}
	foreach($name in @('createdCount','enumeratedCount','destroyedCount')){$null=RmsUint $hostFacts $name 1 1}
	foreach($name in @('createResult','enumerateCountResult','enumerateResult','enabledExtensionCount','enabledLayerCount')){$null=RmsUint $hostFacts $name 0 0}
	$null=RmsUint $hostFacts physicalDeviceCount 1;$identity=RmsVulkanHandle $hostFacts instanceHandle
	RmsRequire ($identity -cmatch '^[1-9][0-9]*$') 'Host handle must use canonical positive decimal identity'
	RmsRequire ((RmsField $hostFacts cleanupResult) -ceq 'ReturnedNormally' -and (RmsField $hostFacts coverageModel) -ceq 'RetainedFixtureHostInstance') 'Host instance cleanup/coverage facts are missing or relabelled'
	return $identity
}
function RmsCheckVulkanSession($Receipt,[int]$Iterations,[bool]$HostInstanceRequested=$false){
	$version=if($HostInstanceRequested){3}else{2}
	RmsRequire ($Iterations -gt 0 -and (RmsUint $Receipt vulkanEvidenceVersion $version $version) -eq $version) 'Vulkan evidence differs from the requested host-lifetime protocol'
	$hostPresent=($Receipt -is [Collections.IDictionary] -and $Receipt.Contains('hostInstance')) -or $null -ne $Receipt.PSObject.Properties['hostInstance']
	RmsRequire ($hostPresent -eq $HostInstanceRequested) 'Unrequested or missing Vulkan host-instance setup'
	$hostIdentity=$null
	if($HostInstanceRequested){
		# This is a distinct declared compatibility lifetime, not an overlay fix.
		RmsRequire ($Iterations -ge 100) 'Host-instance qualification requires at least100 independent Engine contexts'
		$hostIdentity=RmsCheckHostInstance $Receipt
	}
	$s=RmsField $Receipt windowSession;foreach($name in @('windowInitializeCount','windowShutdownCount')){$null=RmsUint $s $name 1 1}
	foreach($name in @('windowCleanupCompleted','windowHidden','handleCleared')){RmsVulkanFlag $s $name};$identity=RmsVulkanHandle $s windowIdentity
	$null=RmsUint $Receipt allSessionValidationErrors 0 0
	foreach($name in @('allSessionLog','validationCallbackLog')){$null=RmsField $Receipt $name;RmsRequire ($Receipt.$name -is [Array]) "Vulkan session array missing: $name"}
	$records=@{};$boundaries=@();$callbacks=@();$previous=0UL
	foreach($entry in $Receipt.allSessionLog){
		$sequence=RmsUint $entry sequence 1;RmsRequire ($sequence -gt $previous) 'Session log sequence regressed or repeated';$previous=$sequence;$records[[string]$sequence]=$entry
		$null=RmsUint $entry iteration 0 $Iterations;RmsRequire ((RmsField $entry message) -is [string]) 'Session log message missing'
		$kind=RmsField $entry eventKind;$phase=RmsField $entry phase
		if($kind -ceq 'fixtureBoundary'){
			$allowedBoundaries=@('WindowInitializeBegin','WindowInitializeEnd','IterationBegin','IterationEnd','WindowShutdownBegin','WindowShutdownEnd');if($HostInstanceRequested){$allowedBoundaries+=@('HostInstanceInitializeBegin','HostInstanceInitializeEnd','HostInstanceShutdownBegin','HostInstanceShutdownEnd')}
			RmsRequire ($phase -cin $allowedBoundaries -and $null -eq (RmsField $entry level) -and (RmsField $entry succeeded) -is [bool]) 'Invalid actual fixture boundary'
			$boundaries+=,$entry
		}else{
			$allowedPhases=@('WindowInitialize','Iteration','BetweenIterations','WindowShutdown');if($HostInstanceRequested){$allowedPhases+=@('HostInstanceInitialize','HostInstanceShutdown')}
			RmsRequire ($kind -ceq 'logger' -and $phase -cin $allowedPhases) 'Unrecognized actual logger record'
			RmsRequire (($phase -ceq 'Iteration' -and $entry.iteration -gt 0) -or ($phase -cne 'Iteration' -and $entry.iteration -eq 0)) 'Logger phase/iteration scope is inconsistent'
			$level=RmsUint $entry level 0 4
			if($entry.message.Contains('[Vulkan]')){RmsRequire ($level -lt 3) 'Actual Vulkan error arrived during the complete window session';$callbacks+=,$entry}
		}
	}
	$extraBoundaries=if($HostInstanceRequested){8}else{4}
	RmsRequire ($boundaries.Count -eq 2*$Iterations+$extraBoundaries -and $boundaries[0].phase -ceq 'WindowInitializeBegin' -and $boundaries[-1].phase -ceq 'WindowShutdownEnd') 'Session sink did not span initialization through final shutdown'
	RmsRequire ($Receipt.allSessionLog[0].phase -ceq 'WindowInitializeBegin' -and $Receipt.allSessionLog[-1].phase -ceq 'WindowShutdownEnd') 'Vulkan sink records fall outside the actual window session boundaries'
	$hostBoundaries=@{};foreach($phase in @('WindowInitializeBegin','WindowInitializeEnd','WindowShutdownBegin','WindowShutdownEnd')){$found=@($boundaries|Where-Object {$_.phase -ceq $phase -and $_.iteration -eq 0});RmsRequire ($found.Count -eq 1) "Missing/repeated actual session boundary: $phase";$hostBoundaries[$phase]=$found[0]}
	RmsVulkanFlag $hostBoundaries.WindowInitializeEnd succeeded;RmsVulkanFlag $hostBoundaries.WindowShutdownEnd succeeded
	RmsRequire ($hostBoundaries.WindowInitializeBegin.sequence -lt $hostBoundaries.WindowInitializeEnd.sequence -and $hostBoundaries.WindowInitializeEnd.sequence -lt $hostBoundaries.WindowShutdownBegin.sequence -and $hostBoundaries.WindowShutdownBegin.sequence -lt $hostBoundaries.WindowShutdownEnd.sequence) 'Actual host lifecycle boundaries are out of order'
	$previous=$hostBoundaries.WindowInitializeEnd.sequence
	$shutdownBoundary=$hostBoundaries.WindowShutdownBegin.sequence
	if($HostInstanceRequested){
		foreach($phase in @('HostInstanceInitializeBegin','HostInstanceInitializeEnd','HostInstanceShutdownBegin','HostInstanceShutdownEnd')){
			$found=@($boundaries|Where-Object {$_.phase -ceq $phase -and $_.iteration -eq 0});RmsRequire ($found.Count -eq 1) "Missing/repeated host-instance boundary: $phase";RmsVulkanFlag $found[0] succeeded;$hostBoundaries[$phase]=$found[0]
		}
		RmsRequire ($hostBoundaries.WindowInitializeEnd.sequence -lt $hostBoundaries.HostInstanceInitializeBegin.sequence -and $hostBoundaries.HostInstanceInitializeBegin.sequence -lt $hostBoundaries.HostInstanceInitializeEnd.sequence -and $hostBoundaries.HostInstanceInitializeEnd.sequence -lt $hostBoundaries.HostInstanceShutdownBegin.sequence -and $hostBoundaries.HostInstanceShutdownBegin.sequence -lt $hostBoundaries.HostInstanceShutdownEnd.sequence -and $hostBoundaries.HostInstanceShutdownEnd.sequence -lt $hostBoundaries.WindowShutdownBegin.sequence) 'Host instance did not span Engine contexts and retire before GLFW loader shutdown'
		$previous=$hostBoundaries.HostInstanceInitializeEnd.sequence;$shutdownBoundary=$hostBoundaries.HostInstanceShutdownBegin.sequence
	}
	for($i=1;$i -le $Iterations;$i++){$begin=@($boundaries|Where-Object {$_.phase -ceq 'IterationBegin' -and $_.iteration -eq $i});$end=@($boundaries|Where-Object {$_.phase -ceq 'IterationEnd' -and $_.iteration -eq $i});RmsRequire ($begin.Count -eq 1 -and $end.Count -eq 1 -and $begin[0].sequence -gt $previous -and $begin[0].sequence -lt $end[0].sequence -and $end[0].sequence -lt $shutdownBoundary) 'Vulkan iteration boundaries are missing/reordered';RmsVulkanFlag $end[0] succeeded;$previous=$end[0].sequence}
	RmsRequire ($Receipt.validationCallbackLog.Count -eq $callbacks.Count) 'Session callback slice omitted actual native messages'
	for($i=0;$i -lt $callbacks.Count;$i++){RmsRequire ((RmsStableJson $Receipt.validationCallbackLog[$i]) -ceq (RmsStableJson $callbacks[$i])) 'Session callback does not match actual logger provenance'}
	return [pscustomobject]@{windowIdentity=$identity;records=$records;hostInstanceRequested=$HostInstanceRequested;hostInstanceIdentity=$hostIdentity}
}
function RmsCheckVulkanRowSession($Row,$Session){
	# A retained window must not claim per-iteration destruction. The final root
	# receipt observes window destruction only after every context owner retires.
	$obsolete=($Row -is [Collections.IDictionary] -and $Row.Contains('windowCleanupCompleted')) -or $null -ne $Row.PSObject.Properties['windowCleanupCompleted']
	RmsRequire (!$obsolete) 'Retained-window iteration cannot claim per-iteration window cleanup'
	$shared=RmsField $Row sharedWindow;RmsRequire ((RmsVulkanHandle $shared windowIdentity) -ceq $Session.windowIdentity) 'Vulkan iteration used a foreign window'
	foreach($name in @('windowRetainedForSession','liveBeforeContextInitialization','liveAfterContextCleanup','hiddenBeforeContextInitialization','hiddenAfterContextCleanup')){RmsVulkanFlag $shared $name}
	$lifetime=RmsField $Row contextLifetime;RmsRequire ((RmsUint $lifetime fixtureLifetimeId 1) -eq $Row.iteration) 'Vulkan context scope identity differs from its iteration'
	RmsVulkanFlag $lifetime constructed;RmsVulkanFlag $lifetime destructionCompleted
	foreach($name in @('instanceHandle','deviceHandle')){$null=RmsVulkanHandle $lifetime $name}
	$rowHostPresent=($Row -is [Collections.IDictionary] -and $Row.Contains('hostInstance')) -or $null -ne $Row.PSObject.Properties['hostInstance']
	RmsRequire ($rowHostPresent -eq $Session.hostInstanceRequested) 'Vulkan row has unrequested or missing host-instance evidence'
	if($Session.hostInstanceRequested){
		$hostFacts=RmsField $Row hostInstance
		$contextIdentity=RmsVulkanHandle $lifetime instanceHandle
		RmsRequire ($contextIdentity -cmatch '^[1-9][0-9]*$' -and (RmsVulkanHandle $hostFacts instanceHandle) -ceq $Session.hostInstanceIdentity -and $contextIdentity -cne $Session.hostInstanceIdentity) 'Engine context shared or changed the live host instance'
		foreach($name in @('liveBeforeContextInitialization','liveAfterContextCleanup','distinctFromContextInstance')){RmsVulkanFlag $hostFacts $name}
	}
	foreach($name in @('allSessionLog','validationCallbackLog')){
		$null=RmsField $Row $name;RmsRequire ($Row.$name -is [Array]) "Vulkan iteration log array missing: $name";$last=0UL
		foreach($entry in $Row.$name){$sequence=RmsUint $entry sequence 1;RmsRequire ($sequence -gt $last -and $entry.iteration -eq $Row.iteration -and $Session.records.ContainsKey([string]$sequence) -and (RmsStableJson $entry) -ceq (RmsStableJson $Session.records[[string]$sequence])) 'Vulkan iteration log is not an actual ordered session slice';$last=$sequence;if($name -ceq 'validationCallbackLog'){RmsRequire ($entry.eventKind -ceq 'logger' -and $entry.message.Contains('[Vulkan]')) 'Iteration callback lacks actual native logger provenance'}}
		$expected=@($Session.records.Values|Where-Object {$_.iteration -eq $Row.iteration -and ($name -ceq 'allSessionLog' -or ($_.eventKind -ceq 'logger' -and $_.message.Contains('[Vulkan]')))})
		RmsRequire ($Row.$name.Count -eq $expected.Count) 'Vulkan iteration omitted records from its actual contiguous session slice'
	}
}
function RmsCheckVulkanObservation($Row,[string]$Scenario,[bool]$RequireBounds=$true){
	foreach($key in @('graphicsInstantiated','validationActuallyEnabled','contextCleanupCompleted','threadPoolCleanupCompleted')){RmsVulkanFlag $Row $key}
	if(!$RequireBounds){RmsVulkanFlag $Row windowCleanupCompleted}
	# Shutdown returning does not prove that native owners were released. Require
	# its actual attempt and the post-call initialized getter, including its type.
	# Old retained smoke can omit these fields only during explicit parser replay;
	# any supplied contradictory or nullable observation must still be rejected.
	foreach($key in @('contextShutdownAttempted','contextInitializedAfterShutdown')){
		$present=($Row -is [Collections.IDictionary] -and $Row.Contains($key)) -or $null -ne $Row.PSObject.Properties[$key]
		if(!$present -and !$RequireBounds){continue}
		$value=RmsField $Row $key;$expected=$key -ceq 'contextShutdownAttempted'
		RmsRequire ($value -is [bool] -and $value -eq $expected) "Vulkan observed context teardown proof missing or invalid: $key"
	}
	$null=RmsUint $Row threadPoolWorkerCount 1
	RmsRequire ((RmsField $Row 'allSessionValidationErrors') -eq 0) 'Vulkan validation errors were observed'
	$adapter=RmsField $Row 'adapter';foreach($key in @('hardwareName','driverVersion','apiVersion')){RmsRequire (![string]::IsNullOrWhiteSpace((RmsField $adapter $key))) 'Vulkan adapter/driver identity missing'}
	$session=RmsUint $adapter renderSessionGeneration 1;$null=RmsField $Row 'validationCallbackLog';$null=RmsField $Row 'allSessionLog'
	$op=RmsField $Row 'operation';RmsCheckVkIndices $op $RequireBounds;RmsCheckVkResults $op
	RmsRequire ((RmsField $op 'acquired') -eq $true -and (RmsField $op 'forcedEmpty') -eq $true -and (RmsField $op 'correlationId') -gt 0 -and (RmsField $op 'maintenanceEnabled') -eq (RmsField $Row 'maintenanceEnabled') -and (RmsField $op 'validationErrorsBefore') -eq 0 -and (RmsField $op 'validationErrorsAfter') -eq 0) 'No valid actual forced-empty acquisition'
	RmsRequire ((RmsField $op 'waitIdleAttempted') -eq $true -and (RmsField $op 'waitIdleCompletionObserved') -eq $true -and (RmsField $op 'waitIdleResult') -eq 0) 'Operation retirement was not actually observed'
	if($Scenario -in @('record-failure','fence-reset-failure','submit-failure')){
		$checkpoint=@{'record-failure'=1;'fence-reset-failure'=2;'submit-failure'=3}[$Scenario]
		RmsRequire ((RmsField $op 'rejectedCheckpoint') -eq $checkpoint -and (RmsField $op 'recoverySuspended') -eq $true -and (RmsField $op 'acquisitionDrainSubmitted') -eq $true -and (RmsField $op 'acquisitionDrainCompleted') -eq $true -and (RmsField $op 'submitAttempted') -eq $false -and (RmsField $op 'submitAccepted') -eq $false -and (RmsField $op 'fallbackCommandSubmitted') -eq $false -and (RmsField $op 'presentAttempted') -eq $false) 'Missing actual injected checkpoint/drain proof'
		RmsRequire ((RmsField $op 'recordEnded') -eq ($checkpoint -ge 2) -and (RmsField $op 'resetAttempted') -eq ($checkpoint -eq 3)) 'Injected checkpoint executed a forbidden later operation'
		if($checkpoint -eq 3){RmsRequire ((RmsField $op 'resetResult') -eq 0 -and (RmsField $op 'fenceResetAccepted') -eq $true) 'Submit checkpoint lacks actual reset proof'}
	}else{
		foreach($key in @('recordEnded','resetAttempted','fenceResetAccepted','submitAttempted','submitAccepted','fallbackCommandSubmitted','presentAttempted')){RmsRequire ((RmsField $op $key) -eq $true) "Actual fallback proof missing: $key"}
		RmsRequire ((RmsField $op 'commandBufferCount') -eq 1 -and (RmsField $op 'resetResult') -eq 0 -and (RmsField $op 'submitResult') -eq 0 -and (RmsField $op 'presentResult') -in @(0,1000001003) -and (RmsField $op 'rejectedCheckpoint') -eq 0 -and (RmsField $op 'recoverySuspended') -eq $false) 'Fallback submit/present failed'
	}
	if($Scenario -ceq 'fresh-acquisition'){RmsRequire ($op.generation -eq 1 -and $op.sequence -eq 1) 'Fresh lane did not use first acquired generation'}
	if($Scenario -ceq 'recreated-acquisition'){RmsRequire ((RmsField $Row 'recreatedBeforeFirstAcquisition') -eq $true -and $op.generation -gt 1 -and $op.sequence -eq 1) 'Recreated lane lacks actual recreation before first acquisition'}
	if($Scenario -ceq 'acquire-wait-consumed'){RmsRequire ((RmsField $op 'acquireWaitConsumedBefore') -eq $true -and (RmsField $op 'acquireWaitConsumed') -eq $true -and (RmsField $op 'acquisitionDrainSubmitted') -eq $true -and (RmsField $op 'acquisitionDrainCompleted') -eq $true) 'Consumed acquire wait lacks actual prior drain'}
	if($Scenario -ceq 'maintenance-enabled'){RmsRequire ($Row.maintenanceEnabled -eq $true) 'Requested maintenance was not actually enabled'}
	if($Scenario -ceq 'maintenance-disabled'){RmsRequire ($Row.maintenanceEnabled -eq $false) 'Requested maintenance-off lane was not actually disabled'}
	if($Scenario -ceq 'previously-presented'){
		$prelude=@(RmsField $Row 'preludeFrames');RmsRequire ($prelude.Count -ge 1) 'Previously-presented lane lacks actual prelude'
		$matched=$false;foreach($n in $prelude){RmsCheckVkIndices $n $RequireBounds;RmsCheckVkResults $n;$completed=($n.fallbackCommandSubmitted -and $n.submitAccepted -and $n.waitIdleCompletionObserved) -or ($n.ordinarySubmitAccepted -and $n.ordinarySubmissionSerial -gt 0 -and $n.ordinaryCompletionObserved);if($n.generation -eq $op.generation -and $n.imageIndex -eq $op.imageIndex -and $n.sequence -lt $op.sequence -and $completed -and $n.presentAttempted -and $n.presentResult -in @(0,1000001003)){$matched=$true}}
		RmsRequire $matched 'Selected empty image has no earlier completed presentation'
	}
	RmsCheckHealthyFrames (RmsField $Row 'healthyFrames') $op $session $RequireBounds
	if($Scenario -in @('record-failure','fence-reset-failure','submit-failure')){RmsRequire ($Row.healthyFrames[0].native.generation -gt $op.generation) 'Failed acquisition was reused without native recovery'}
	if($Scenario -ceq 'candidate-rollback' -or ($Scenario -ceq 'maintenance-disabled' -and $RequireBounds)){
		$subcases=@(RmsField $Row 'candidateSubcases');RmsRequire ($subcases.Count -eq 2) 'Candidate rollback requires both actual checkpoints'
		$before=@($subcases|Where-Object {$_.checkpoint -ceq 'BeforeSwapchain'});$partial=@($subcases|Where-Object {$_.checkpoint -ceq 'AfterFallbackPass'})
		RmsRequire ($before.Count -eq 1 -and $partial.Count -eq 1) 'Candidate rollback checkpoints missing or duplicated'
		$b=$before[0];$p=$partial[0];RmsCheckVkResults $b.native;RmsCheckVkResults $p.native
		if($p.native.acquired){RmsCheckVkIndices $p.native $RequireBounds}
		RmsRequire ($b.replacementPublished -eq $false -and $b.native.candidateRejected -eq $true -and $b.native.candidateCheckpoint -eq 1 -and $b.native.candidateOldSwapchainConsumed -eq $false) 'Pre-handoff rejection did not preserve native ownership'
		RmsRequire ($p.replacementPublished -eq $false -and $p.native.candidateRejected -eq $true -and $p.native.candidateCheckpoint -eq 2 -and $p.native.candidateOldSwapchainConsumed -eq $true -and $p.native.candidateFallbackPassCreated -eq $true -and $p.native.candidatePartialCleanupCompleted -eq $true -and $p.native.recoverySuspended -eq $true) 'Partial candidate did not fail closed with actual cleanup'
		if($p.outcome -ceq 'RecoveredAfterNativeRetirementProof'){RmsCheckHealthyFrames (RmsField $p 'healthyFrames') $p.native $session $RequireBounds;RmsRequire ($p.healthyFrames[0].native.generation -gt $p.native.generation) 'Partial candidate recovery reused consumed generation'}
		elseif($p.outcome -ceq 'FailClosedTerminalShutdownRequired'){
			$blocked=RmsField $p 'blockedBeginFrame';RmsCheckVkIndices $blocked $RequireBounds;RmsCheckVkResults $blocked
			RmsRequire ($Row.contextCleanupCompleted -eq $true -and @($p.healthyFrames).Count -eq 0 -and $blocked.recoverySuspended -eq $true -and $blocked.sequence -eq $p.native.sequence -and $blocked.ordinarySubmissionSerial -eq $p.native.ordinarySubmissionSerial -and $blocked.generation -eq $p.native.generation) 'Partial candidate terminal cleanup or blocked-acquisition proof missing'
		}
		else{throw 'UNVERIFIED: Unknown partial-candidate retirement outcome'}
		if($Scenario -ceq 'maintenance-disabled'){RmsRequire ($p.native.maintenanceEnabled -eq $false -and $p.outcome -ceq 'FailClosedTerminalShutdownRequired') 'Unextended consumed-old handoff did not stay fail closed until terminal shutdown'}
	}
}
function RmsCheckRendererEpisode($Episode,[bool]$Pressure=$false){
	foreach($key in @('lifetimeId','episodeId','frameId')){$null=RmsUint $Episode $key 1}
	RmsRequire ((RmsField $Episode 'capacityBytes') -eq 16777216) 'Renderer capture changed the16MiB backing contract'
	foreach($key in @('usedBytes','peakBytes','lifetimeHighWaterBytes','reuseAllocationCount','fallbackAllocationCount','resetCount')){$null=RmsUint $Episode $key}
	RmsRequire ($Episode.peakBytes -ge $Episode.usedBytes -and $Episode.lifetimeHighWaterBytes -ge $Episode.peakBytes) 'Renderer pressure/high-water counters are inconsistent'
	if($Pressure){RmsRequire ($Episode.usedBytes -gt 0 -and $Episode.fallbackAllocationCount -gt 0) 'Renderer pressure did not produce actual fallback'}
}
function RmsCheckRendererTerminal($State,$Captured,[int]$Outcome,[bool]$MustBeInactive=$true){
	if($MustBeInactive){RmsRequire ((RmsField $State 'active') -eq $false) 'Terminal renderer episode is still active'}
	$matches=@(@((RmsField $State 'latest'),(RmsField $State 'previous'))|Where-Object {$_.lifetimeId -eq $Captured.lifetimeId -and $_.episodeId -eq $Captured.episodeId -and $_.frameId -eq $Captured.frameId -and $_.outcome -eq $Outcome})
	RmsRequire ($matches.Count -eq 1) 'Copied terminal episode lost its actual identity/outcome'
	RmsCheckRendererEpisode $matches[0] $true
	RmsRequire ($matches[0].fallbackAllocationCount -ge $Captured.fallbackAllocationCount -and $matches[0].peakBytes -ge $Captured.peakBytes) 'Terminal copy lost pre-reset pressure facts'
}
function RmsCheckRendererBalance($State,[uint64]$Lifetime){
	RmsRequire ((RmsField $State 'active') -eq $false) 'Protected clear retained a live scratch lease'
	$r=RmsField $State 'reclamation'
	RmsRequire ((RmsField $r 'lifetimeId') -eq $Lifetime -and (RmsField $r 'totalFallbackAllocationCount') -gt 0 -and $r.totalFallbackAllocationCount -eq (RmsField $r 'totalFallbackDeallocationCount') -and (RmsField $r 'outstandingFallbackAllocationCount') -eq 0 -and (RmsField $r 'outstandingFallbackBytes') -eq 0) 'Actual fallback ownership did not balance for the same renderer lifetime'
	$null=RmsUint $r resetCount
}
function RmsRendererFlag($Value,[string]$Name,[bool]$Expected=$true){$flag=RmsField $Value $Name;RmsRequire ($flag -is [bool] -and $flag -eq $Expected) "Renderer lifecycle observation missing/invalid: $Name"}
function RmsCheckRendererSession($Receipt,[int]$Iterations,[bool]$HostInstanceRequested=$false){
	$version=if($HostInstanceRequested){3}else{2}
	RmsRequire ($Iterations -gt 0 -and (RmsUint $Receipt rendererEvidenceVersion $version $version) -eq $version) 'Renderer evidence differs from requested one-engine-session protocol'
	foreach($name in @('hostInstance','rendererContexts')){
		$present=($Receipt -is [Collections.IDictionary] -and $Receipt.Contains($name)) -or $null -ne $Receipt.PSObject.Properties[$name]
		RmsRequire ($present -eq $HostInstanceRequested) 'Renderer host setup is missing or was not requested'
	}
	$hostIdentity=$null;$contextRecords=@()
	if($HostInstanceRequested){
		RmsRequire ($Iterations -ge 100) 'Renderer host-instance qualification requires at least100 lifetimes'
		$hostIdentity=RmsCheckHostInstance $Receipt;$proof=RmsField $Receipt rendererContexts
		foreach($name in @('finalRendererShutdownAttempted','finalContextReleased','windowLiveBeforeHostClose')){RmsVulkanFlag $proof $name}
		$contextRecords=RmsField $proof contexts;RmsRequire ($contextRecords -is [Array]) 'Actual renderer context observations missing'
		$perIteration=if((RmsField $Receipt scenario) -ceq 'restart'){2}else{1};$expectedCount=1+$Iterations*$perIteration
		RmsRequire ($contextRecords.Count -eq $expectedCount) 'Renderer observed context lifecycle closure incomplete'
		$null=RmsUint $proof observedInitializationCount $expectedCount $expectedCount;$null=RmsUint $proof observedRetirementCount $expectedCount $expectedCount
		for($index=0;$index -lt $contextRecords.Count;$index++){
			$row=$contextRecords[$index];$role=if($index -eq 0){'Startup'}elseif($perIteration -eq 2 -and $index%2 -eq 0){'Restart'}else{'Primary'}
			$iteration=if($index -eq 0){0}else{1+[int][Math]::Floor(($index-1)/$perIteration)}
			RmsRequire ((RmsField $row role) -ceq $role -and (RmsUint $row iteration 0 $Iterations) -eq $iteration) 'Renderer context role/iteration order differs'
			foreach($name in @('initialized','validationActuallyEnabled','hostLiveAfter','distinctFromHost','shutdownAttempted','contextReleased')){RmsVulkanFlag $row $name}
			RmsVulkanFlag $row hostLiveBefore ($role -cne 'Startup')
			$checkpoint=if($role -ceq 'Startup'){'ExistingStartupContextAfterHostCreation'}else{'ContextInitializedUnderHost'}
			RmsRequire ((RmsField $row observationCheckpoint) -ceq $checkpoint) 'Renderer startup/host creation order was relabelled'
			$contextIdentity=RmsVulkanHandle $row instanceHandle
			RmsRequire ($contextIdentity -cmatch '^[1-9][0-9]*$' -and $contextIdentity -cne $hostIdentity) 'Renderer shared the live host instance';$null=RmsVulkanHandle $row deviceHandle
		}
	}
	$s=RmsField $Receipt hostSession;foreach($name in @('engineInitializeCount','engineShutdownCount')){$null=RmsUint $s $name 1 1};RmsRendererFlag $s engineCleanupCompleted
	$workers=RmsUint $s threadPoolWorkerCount 1;$null=RmsUint $s threadPoolWorkersAfterCleanup 0 0;RmsRequire ($null -eq (RmsField $s frameFailure)) 'Final host frame failure was not cleared'
	$null=RmsUint $Receipt allSessionValidationErrors 0 0
	foreach($name in @('allSessionLog','validationCallbackLog','capturedLifetimeIds')){$null=RmsField $Receipt $name;RmsRequire ($Receipt.$name -is [Array]) "Renderer session array missing: $name"}
	RmsRequire ($Receipt.capturedLifetimeIds.Count -eq $Iterations) 'Session captured lifetime count differs from requested iterations'
	$ids=@{};$previous=0UL;foreach($id in $Receipt.capturedLifetimeIds){$value=RmsUint ([pscustomobject]@{id=$id}) id 1;RmsRequire (!$ids.ContainsKey([string]$value) -and $value -gt $previous) 'Captured lifetimes repeated or regressed within the process';$ids[[string]$value]=$true;$previous=$value}
	$records=@{};$boundaries=@();$callbacks=@();$previous=0UL
	foreach($entry in $Receipt.allSessionLog){
		$sequence=RmsUint $entry sequence 1;RmsRequire ($sequence -gt $previous) 'Session log sequence regressed or repeated';$previous=$sequence;$records[[string]$sequence]=$entry
		$null=RmsUint $entry iteration 0 $Iterations;RmsRequire ((RmsField $entry message) -is [string]) 'Session log message missing'
		$kind=RmsField $entry eventKind;$phase=RmsField $entry phase
		if($kind -ceq 'fixtureBoundary'){
			$allowed=@('HostInitializeBegin','HostInitializeEnd','IterationBegin','IterationEnd','HostShutdownBegin','HostShutdownEnd');if($HostInstanceRequested){$allowed+=@('HostInstanceInitializeBegin','HostInstanceInitializeEnd','StartupRendererShutdownBegin','StartupRendererShutdownEnd','HostInstanceShutdownBegin','HostInstanceShutdownEnd')}
			RmsRequire ($phase -cin $allowed -and $null -eq (RmsField $entry level) -and (RmsField $entry succeeded) -is [bool]) 'Invalid actual fixture boundary'
			$boundaries+=,$entry
		}else{
			$allowed=@('HostInitialize','Iteration','BetweenIterations','HostShutdown');if($HostInstanceRequested){$allowed+=@('HostInstanceInitialize','StartupRendererShutdown','HostInstanceShutdown')}
			RmsRequire ($kind -ceq 'logger' -and $phase -cin $allowed) 'Unrecognized actual logger record'
			RmsRequire (($phase -ceq 'Iteration' -and $entry.iteration -gt 0) -or ($phase -cne 'Iteration' -and $entry.iteration -eq 0)) 'Logger phase/iteration scope is inconsistent'
			$level=RmsUint $entry level 0 4
			if($entry.message.Contains('[Vulkan]')){RmsRequire ($level -lt 3) 'Actual Vulkan error arrived during the complete engine session';$callbacks+=,$entry}
		}
	}
	$extra=if($HostInstanceRequested){10}else{4}
	RmsRequire ($boundaries.Count -eq 2*$Iterations+$extra -and $boundaries[0].phase -ceq 'HostInitializeBegin' -and $boundaries[-1].phase -ceq 'HostShutdownEnd') 'Session sink did not span initialization through final shutdown'
	$hostBoundaries=@{};foreach($phase in @('HostInitializeBegin','HostInitializeEnd','HostShutdownBegin','HostShutdownEnd')){$found=@($boundaries|Where-Object {$_.phase -ceq $phase -and $_.iteration -eq 0});RmsRequire ($found.Count -eq 1) "Missing/repeated actual session boundary: $phase";$hostBoundaries[$phase]=$found[0]}
	RmsRendererFlag $hostBoundaries.HostInitializeEnd succeeded;RmsRendererFlag $hostBoundaries.HostShutdownEnd succeeded
	RmsRequire ($hostBoundaries.HostInitializeBegin.sequence -lt $hostBoundaries.HostInitializeEnd.sequence -and $hostBoundaries.HostInitializeEnd.sequence -lt $hostBoundaries.HostShutdownBegin.sequence -and $hostBoundaries.HostShutdownBegin.sequence -lt $hostBoundaries.HostShutdownEnd.sequence) 'Actual host lifecycle boundaries are out of order'
	$previous=$hostBoundaries.HostInitializeEnd.sequence
	$shutdown=$hostBoundaries.HostShutdownBegin.sequence
	if($HostInstanceRequested){
		foreach($phase in @('HostInstanceInitializeBegin','HostInstanceInitializeEnd','StartupRendererShutdownBegin','StartupRendererShutdownEnd','HostInstanceShutdownBegin','HostInstanceShutdownEnd')){
			$found=@($boundaries|Where-Object {$_.phase -ceq $phase -and $_.iteration -eq 0});RmsRequire ($found.Count -eq 1) "Missing/repeated renderer host boundary: $phase";RmsRendererFlag $found[0] succeeded;$hostBoundaries[$phase]=$found[0]
		}
		$order=@('HostInitializeEnd','HostInstanceInitializeBegin','HostInstanceInitializeEnd','StartupRendererShutdownBegin','StartupRendererShutdownEnd','HostInstanceShutdownBegin','HostInstanceShutdownEnd','HostShutdownBegin')
		for($i=1;$i -lt $order.Count;$i++){RmsRequire ($hostBoundaries[$order[$i-1]].sequence -lt $hostBoundaries[$order[$i]].sequence) 'Renderer host/startup/engine shutdown boundaries reordered'}
		$previous=$hostBoundaries.StartupRendererShutdownEnd.sequence;$shutdown=$hostBoundaries.HostInstanceShutdownBegin.sequence
	}
	for($i=1;$i -le $Iterations;$i++){$begin=@($boundaries|Where-Object {$_.phase -ceq 'IterationBegin' -and $_.iteration -eq $i});$end=@($boundaries|Where-Object {$_.phase -ceq 'IterationEnd' -and $_.iteration -eq $i});RmsRequire ($begin.Count -eq 1 -and $end.Count -eq 1 -and $begin[0].sequence -gt $previous -and $begin[0].sequence -lt $end[0].sequence -and $end[0].sequence -lt $shutdown) 'Renderer iteration boundaries are missing/reordered';RmsRendererFlag $end[0] succeeded;$previous=$end[0].sequence}
	RmsRequire ($Receipt.validationCallbackLog.Count -eq $callbacks.Count) 'Session callback slice omitted actual native messages'
	for($i=0;$i -lt $callbacks.Count;$i++){RmsRequire ((RmsStableJson $Receipt.validationCallbackLog[$i]) -ceq (RmsStableJson $callbacks[$i])) 'Session callback does not match actual logger provenance'}
	return [pscustomobject]@{workers=$workers;records=$records;capturedLifetimeIds=$Receipt.capturedLifetimeIds;hostInstanceRequested=$HostInstanceRequested;hostInstanceIdentity=$hostIdentity;contextRecords=$contextRecords}
}
function RmsCheckRendererRowSession($Row,$Session){
	$present=($Row -is [Collections.IDictionary] -and $Row.Contains('hostInstance')) -or $null -ne $Row.PSObject.Properties['hostInstance']
	RmsRequire ($present -eq $Session.hostInstanceRequested) 'Renderer row has hidden or missing host-instance proof'
	if($Session.hostInstanceRequested){
		$hostFacts=RmsField $Row hostInstance;RmsRequire ((RmsVulkanHandle $hostFacts instanceHandle) -ceq $Session.hostInstanceIdentity) 'Renderer row host identity changed'
		foreach($name in @('liveBeforeContextInitialization','liveAfterContextCleanup')){RmsVulkanFlag $hostFacts $name}
		# Read the property directly so a single native context remains an array, while scalar forgeries still fail.
		$null=RmsField $hostFacts contextLifecycles
		if($hostFacts -is [Collections.IDictionary]){$actual=$hostFacts['contextLifecycles']}else{$actual=$hostFacts.contextLifecycles}
		$expected=@($Session.contextRecords|Where-Object iteration -EQ $Row.iteration)
		RmsRequire ($actual -is [Array] -and $actual.Count -eq $expected.Count -and (RmsStableJson $actual) -ceq (RmsStableJson $expected)) 'Renderer row contexts do not join actual root lifetime observations'
	}
	RmsRequire ($Row.threadPoolWorkerCount -eq $Session.workers -and $Row.rendererCleanup.workerCountAfterRetirement -eq $Session.workers) 'Renderer iteration prematurely changed the shared host worker pool'
	foreach($name in @('allSessionLog','validationCallbackLog')){
		$null=RmsField $Row $name;RmsRequire ($Row.$name -is [Array]) "Renderer iteration log array missing: $name";$last=0UL
		foreach($entry in $Row.$name){$sequence=RmsUint $entry sequence 1;RmsRequire ($sequence -gt $last -and $entry.iteration -eq $Row.iteration -and $Session.records.ContainsKey([string]$sequence) -and (RmsStableJson $entry) -ceq (RmsStableJson $Session.records[[string]$sequence])) 'Renderer iteration log is not an actual ordered session slice';$last=$sequence;if($name -ceq 'validationCallbackLog'){RmsRequire ($entry.eventKind -ceq 'logger' -and $entry.message.Contains('[Vulkan]')) 'Iteration callback lacks actual native logger provenance'}}
		$expected=@($Session.records.Values|Where-Object {$_.iteration -eq $Row.iteration -and ($name -ceq 'allSessionLog' -or ($_.eventKind -ceq 'logger' -and $_.message.Contains('[Vulkan]')))})
		RmsRequire ($Row.$name.Count -eq $expected.Count) 'Renderer iteration omitted records from its actual contiguous session slice'
	}
}
function RmsCheckRendererObservation($Row,[string]$Scenario){
	foreach($key in @('graphicsInstantiated','validationActuallyEnabled','windowHidden','authoredCapturedParity')){RmsRequire ((RmsField $Row $key) -eq $true) "Actual renderer fixture proof missing: $key"}
	RmsRequire ($null -eq $Row.PSObject.Properties['engineCleanupCompleted'] -and $null -eq $Row.PSObject.Properties['threadPoolWorkersAfterCleanup']) 'Iteration cannot claim whole-engine cleanup in the shared session'
	$null=RmsUint $Row allSessionValidationErrors 0 0
	$startup=RmsField $Row rendererStartup;RmsRendererFlag $startup initialized;RmsRendererFlag $startup validationActuallyEnabled;$null=RmsUint $startup renderSessionGeneration 1
	$cleanup=RmsField $Row rendererCleanup;foreach($key in @('attempted','completed','contextReleased')){RmsRendererFlag $cleanup $key};RmsRendererFlag $cleanup activeAfterRetirement $false;$null=RmsUint $cleanup workerCountAfterRetirement 1
	$null=RmsUint $Row threadPoolWorkerCount 2;$lights=RmsUint $Row authoredLightCount 131072 131072
	$adapter=RmsField $Row 'adapter';foreach($key in @('hardwareName','driverVersion','apiVersion')){RmsRequire (![string]::IsNullOrWhiteSpace((RmsField $adapter $key))) 'Renderer adapter identity missing'}
	$null=RmsField $Row 'validationCallbackLog';$null=RmsField $Row 'allSessionLog'
	$captured=RmsField $Row 'captured';RmsCheckRendererEpisode $captured $true
	RmsRequire ($captured.outcome -eq 1) 'Captured scratch episode was not active'
	if($Scenario -in @('completed','failed')){
		$held=RmsField $Row 'heldRead';foreach($key in @('arrived','completed','activeWhileHeld','phaseUnsettledWhileHeld','protectedClearRejectedWhileHeld')){RmsRequire ((RmsField $held $key) -eq $true) "Held renderer borrower proof missing: $key"}
		$begin=RmsUint $held begin;$end=RmsUint $held end 1 $lights;RmsRequire ($begin -lt $end) 'Held renderer range is invalid'
		$chunks=@(RmsField $Row 'chunks');RmsRequire ($chunks.Count -gt 0) 'Actual native light chunks missing'
		$matching=@($chunks|Where-Object {$_.begin -eq $begin -and $_.end -eq $end -and $_.ended -eq $true -and $_.beforeDigest -eq $held.digest -and $_.afterDigest -eq $held.digest})
		RmsRequire ($matching.Count -eq 1) 'Held child did not read the same actual source before/after release'
		$next=0UL
		foreach($chunk in @($chunks|Sort-Object begin)){
			$b=RmsUint $chunk begin;$e=RmsUint $chunk end 1 $lights;RmsRequire ($b -lt $e) 'Actual native light chunk range invalid'
			$null=RmsField $chunk 'computedSignatureDigest'
			if($chunk.ended){RmsRequire ($chunk.beforeDigest -eq $chunk.afterDigest) 'Settled renderer child input changed while borrowed'}
			if($Scenario -ceq 'completed'){RmsRequire ($chunk.ended -eq $true -and $b -eq $next) 'Completed renderer chunk coverage has a gap, overlap or missing end';$next=$e}
		}
		if($Scenario -ceq 'completed'){RmsRequire ($next -eq $lights -and $held.originalFailure -ceq '') 'Completed renderer preparation failed or omitted authored lights'}
		else{RmsRequire ($held.originalFailure -ceq 'RMS renderer inline failure after held child arrived') 'Original failed renderer preparation error was lost'}
		RmsCheckRendererTerminal (RmsField $Row 'terminal') $captured $(if($Scenario -ceq 'failed'){3}else{2})
	}else{
		$retired=RmsField $Row 'retired';RmsCheckRendererTerminal $retired $captured 4 ($Scenario -cne 'cancelled')
		if($Scenario -ceq 'cancelled'){
			$next=RmsField $Row 'nextCapture';RmsRequire ($next.active -eq $true) 'Cancelled episode replacement has no live lease'
			RmsCheckRendererTerminal $next $captured 4 $false
			# State.latest/previous are terminal copies, never the active lease.
			# The direct replacement capture proves its active identity; the later
			# healthyFollowup independently proves subsequent episode progress.
			$active=RmsField $Row 'nextCaptured';RmsCheckRendererEpisode $active $true;RmsRequire ($active.lifetimeId -eq $captured.lifetimeId -and $active.episodeId -gt $captured.episodeId -and $active.outcome -eq 1) 'Cancellation replaced wrong lifetime or failed to advance episode'
		}else{RmsCheckRendererBalance $retired $captured.lifetimeId}
	}
	if($Scenario -cne 'shutdown'){
		$healthy=RmsField $Row 'healthyFollowup';$newCapture=RmsField $healthy 'captured';RmsCheckRendererEpisode $newCapture $true
		RmsRequire ((RmsField $healthy 'ordinarySubmitAccepted') -eq $true -and (RmsField $healthy 'ordinaryCompletionObserved') -eq $true -and (RmsUint $healthy ordinarySubmissionSerial 1) -gt 0) 'Renderer follow-up lacks actual ordinary submit/completion'
		RmsRequire ((RmsUint $healthy sequence 1) -gt (RmsUint $healthy previousAcquisitionSequence) -and $healthy.ordinarySubmissionSerial -gt (RmsUint $healthy previousOrdinarySubmissionSerial) -and (RmsUint $healthy generation 1) -gt 0) 'Renderer healthy follow-up reused stale acquisition or ordinary submission'
		foreach($key in @('acquired','waitIdleAttempted','waitIdleCompletionObserved','presentAttempted')){RmsRequire ((RmsField $healthy $key) -eq $true) "Renderer healthy follow-up missing actual native flag: $key"}
		RmsRequire ((RmsField $healthy 'commandBufferCount') -gt 0 -and (RmsField $healthy 'forcedEmpty') -eq $false -and (RmsField $healthy 'recoverySuspended') -eq $false -and (RmsField $healthy 'presentResult') -in @(0,1000001003) -and (RmsField $healthy 'waitIdleResult') -eq 0) 'Renderer ordinary follow-up native retirement failed'
		if($Scenario -in @('scene-clear','restart')){RmsRequire ($newCapture.lifetimeId -ne $captured.lifetimeId) 'Renderer clear/restart reused the retired lifetime'}
		elseif($newCapture.lifetimeId -eq $captured.lifetimeId){RmsRequire ($newCapture.episodeId -gt $captured.episodeId) 'Healthy renderer capture reused an old episode'}
		RmsCheckRendererBalance (RmsField $Row 'balancedAfterClear') $newCapture.lifetimeId
	}
	# Renderer retirement happens while the shared engine and pool remain live.
	# Its copied counters must describe the actual final renderer lifetime.
	$retiredLifetime=$(if($Scenario -ceq 'shutdown'){$captured.lifetimeId}else{$newCapture.lifetimeId})
	RmsCheckRendererBalance (RmsField $cleanup retiredState) $retiredLifetime
}
function RmsCanonical($Value){
	if($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]){return $Value}
	if($Value -is [Collections.IDictionary]){$r=[ordered]@{};foreach($key in @($Value.Keys|Sort-Object)){$r[$key]=RmsCanonical $Value[$key]};return $r}
	if($Value -is [Array] -or $Value -is [Collections.IList]){return ,@($Value|ForEach-Object {RmsCanonical $_})}
	if($Value -is [pscustomobject]){$r=[ordered]@{};foreach($key in @($Value.PSObject.Properties|ForEach-Object {$_.Name}|Sort-Object)){$r[$key]=RmsCanonical $Value.$key};return $r}
	return $Value
}
function RmsStableJson($Value){return ConvertTo-Json -InputObject (RmsCanonical $Value) -Depth 100 -Compress}
function RmsCheckEditorScratchSnapshot($Snapshot){
	$names=@('None','Active','Completed','Failed','Cancelled')
	RmsRequire (@($Snapshot.PSObject.Properties|Where-Object {$_.Name -cnotin @('latest','previous','reclamation')}).Count -eq 0) 'Editor scratch diagnostics exposed unexpected control fields'
	foreach($key in @('latest','previous')){
		$episode=RmsField $Snapshot $key;$code=RmsUint $episode outcomeCode 0 4
		RmsRequire ((RmsField $episode 'outcome') -ceq $names[$code]) 'Copied editor outcome string/code disagree'
		$allowed=@('lifetimeId','episodeId','frameId','capacityBytes','usedBytes','peakBytes','lifetimeHighWaterBytes','reuseAllocationCount','fallbackAllocationCount','resetCount','outcome','outcomeCode')
		RmsRequire (@($episode.PSObject.Properties|Where-Object {$allowed -cnotcontains $_.Name}).Count -eq 0) 'Editor diagnostics exposed unexpected resource/allocator controls'
		if($code -eq 0){RmsRequire ($episode.lifetimeId -eq 0 -and $key -ceq 'previous') 'Latest copied renderer episode is unavailable'}
		else{RmsCheckRendererEpisode $episode $false}
	}
	RmsRequire ($Snapshot.latest.outcomeCode -in @(2,3,4)) 'Latest editor renderer copy is not a terminal episode'
	$r=RmsField $Snapshot 'reclamation';foreach($key in @('lifetimeId','totalFallbackAllocationCount','totalFallbackDeallocationCount','outstandingFallbackAllocationCount','outstandingFallbackBytes','resetCount')){$null=RmsUint $r $key}
	RmsRequire ($r.lifetimeId -eq $Snapshot.latest.lifetimeId -and $r.totalFallbackAllocationCount -ge $r.totalFallbackDeallocationCount -and $r.outstandingFallbackAllocationCount -eq ($r.totalFallbackAllocationCount-$r.totalFallbackDeallocationCount) -and ($r.outstandingFallbackAllocationCount -gt 0 -or $r.outstandingFallbackBytes -eq 0)) 'Copied editor reclamation identity/accounting is inconsistent'
}
function RmsCheckEditorLoopback($Receipt,[string]$RuntimeManifestHash,[string]$RuntimeRoot=''){
	RmsRequire ((RmsField $Receipt 'schemaVersion') -ceq 'runtime-memory-safety-editor-loopback-v1' -and $Receipt.status -ceq 'Passed' -and $Receipt.runtimeManifestSha256 -ceq $RuntimeManifestHash -and $Receipt.capability -ceq 'diagnostics.get_summary') 'Installed editor loopback identity is unverified'
	foreach($key in @('endpointOwnershipVerified','isolatedProjectVerified','readOnlyAuthoringStable','playRoundTripAuthoredObjectsExact','authoredSceneFileUnchanged')){RmsRequire ((RmsField $Receipt $key) -eq $true) "Editor live loopback proof missing: $key"}
	RmsRequire ($Receipt.lifecycle.normalClose -eq $true -and $Receipt.lifecycle.exitCode -eq 0 -and $Receipt.lifecycle.timedOut -eq $false) 'Installed editor loopback did not close normally'
	$null=RmsUint $Receipt.process pid 1;$null=RmsField $Receipt.process 'startUtc';$null=RmsField $Receipt.process 'executablePath'
	if($RuntimeRoot){RmsRequire ([IO.Path]::GetFullPath($Receipt.process.executablePath) -ieq (Join-Path $RuntimeRoot 'Editor/TerraEngine-Editor.exe')) 'Editor loopback observed a different runtime executable'}
	RmsRequire (RmsBelow (RmsField $Receipt 'isolatedProjectRoot') (RmsField $Receipt 'workRoot')) 'Editor project is not beneath its isolated owned work root'
	$snapshots=@(RmsField $Receipt 'snapshots');RmsRequire ($snapshots.Count -ge 2) 'At least two actual copied editor snapshots are required';foreach($snapshot in $snapshots){RmsCheckEditorScratchSnapshot $snapshot}
	$raw=@(RmsField $Receipt 'rawEvidence');RmsRequire ($raw.Count -ge 3) 'Editor raw endpoint/lifecycle/authoring evidence missing';foreach($p in $raw){RmsCheckPin $p}
	$rpc=@($raw|Where-Object {[IO.Path]::GetFileName($_.path) -ceq 'rpc.jsonl'});RmsRequire ($rpc.Count -eq 1) 'Unique raw editor RPC log missing'
	$diagnosticCalls=0;$nativeSnapshots=New-Object 'Collections.Generic.List[string]';$callCounts=@{}
	foreach($line in [IO.File]::ReadLines($rpc[0].path)){
		$event=$line|ConvertFrom-Json;$request=$event.request
		if($request.method -ceq 'tools/call'){
			$name=$request.params.name
			if(!$callCounts.ContainsKey($name)){$callCounts[$name]=0};$callCounts[$name]++
			RmsRequire ($name -cin @('project.state','scene.list_objects','diagnostics.get_summary','play.state','play.enter','play.pause','play.exit')) 'Editor loopback performed an unrelated live mutation'
			if($name -ceq 'diagnostics.get_summary'){
				$diagnosticCalls++;$texts=@($event.response.result.content|Where-Object type -eq 'text');RmsRequire ($texts.Count -eq 1) 'Raw diagnostics response lacks unique text envelope';$envelope=$texts[0].text|ConvertFrom-Json
				RmsRequire ($envelope.ok -eq $true) 'Raw diagnostic request failed';if($null -ne $envelope.data.PSObject.Properties['rendererScratch']){$nativeSnapshots.Add((RmsStableJson $envelope.data.rendererScratch))}
			}
		}else{RmsRequire ($request.method -ceq 'initialize') 'Unexpected editor RPC method'}
	}
	RmsRequire ($diagnosticCalls -ge 2) 'Raw RPC log did not observe copied diagnostics twice'
	foreach($key in @('project.state','scene.list_objects','play.state','play.enter','play.pause','play.exit')){RmsRequire ($callCounts.ContainsKey($key)) "Raw editor lifecycle/authoring observation missing: $key"}
	foreach($snapshot in $snapshots){RmsRequire ($nativeSnapshots -ccontains (RmsStableJson $snapshot)) 'Copied diagnostics were not present in actual native RPC responses'}
	$observations=@($raw|Where-Object {[IO.Path]::GetFileName($_.path) -ceq 'observations.json'});RmsRequire ($observations.Count -eq 1) 'Raw authoring/play observations missing'
	$o=Get-Content $observations[0].path -Raw|ConvertFrom-Json
	foreach($key in @('projectGeneration','sceneGeneration','projectRoot','activeScenePath','sceneDirty')){RmsRequire ((RmsStableJson $o.readBefore.$key) -ceq (RmsStableJson $o.readAfter.$key)) 'Raw read-only diagnostics changed authoring/publication state'}
	RmsRequire ((RmsStableJson $o.objectsBeforeRead) -ceq (RmsStableJson $o.objectsAfterRead) -and (RmsStableJson $o.baselineObjects) -ceq (RmsStableJson $o.restoredObjects)) 'Raw authoring objects changed during diagnostics or Play/Stop'
	RmsCheckPin $o.sourceScene
}
function RmsCheckFixture($Receipt,[string]$Mode,[string]$Scenario,[int]$Iterations,[switch]$ProtocolReplayHistoricalReceipt,[switch]$VulkanHostInstance){
	RmsRequire (!$VulkanHostInstance -or ($Mode -cin @('vulkan','renderer') -and !$ProtocolReplayHistoricalReceipt)) 'Host-instance evidence is Vulkan/Renderer-only and cannot use historical protocol replay'
	if($Mode -ceq 'vulkan' -and $ProtocolReplayHistoricalReceipt){
		# Historical schema replay cannot hide the new host-lifetime contract.
		$hostPresent=($Receipt -is [Collections.IDictionary] -and $Receipt.Contains('hostInstance')) -or $null -ne $Receipt.PSObject.Properties['hostInstance']
		$versionPresent=($Receipt -is [Collections.IDictionary] -and $Receipt.Contains('vulkanEvidenceVersion')) -or $null -ne $Receipt.PSObject.Properties['vulkanEvidenceVersion']
		RmsRequire (!$hostPresent -and (!$versionPresent -or $Receipt.vulkanEvidenceVersion -ne 3)) 'Version3 host evidence cannot use historical protocol replay'
		foreach($row in @(RmsField $Receipt observations)){
			$rowHost=($row -is [Collections.IDictionary] -and $row.Contains('hostInstance')) -or $null -ne $row.PSObject.Properties['hostInstance']
			RmsRequire (!$rowHost) 'Historical row cannot hide host-instance evidence'
		}
	}
	RmsRequire ((RmsField $Receipt 'schemaVersion') -eq 1 -and (RmsField $Receipt 'mode') -ceq $Mode -and (RmsField $Receipt 'scenario') -ceq $Scenario) 'Fixture identity mismatch'
	if((RmsField $Receipt 'status') -ceq 'Unverified'){throw 'UNVERIFIED: Native fixture could not establish requested graphics/lifecycle evidence'}
	RmsRequire ((RmsField $Receipt 'status') -ceq 'Passed' -and (RmsField $Receipt 'nativeAcceptance') -eq $true -and (RmsField $Receipt 'completedIterations') -eq $Iterations -and (RmsField $Receipt 'attemptedIterations') -eq $Iterations -and (RmsField $Receipt 'requestedIterations') -eq $Iterations) 'Native fixture did not complete every requested iteration'
	$observations=@(RmsField $Receipt 'observations');RmsRequire ($observations.Count -eq $Iterations) 'Fixture iteration count differs from observations'
	$rendererSession=$null;if($Mode -ceq 'renderer'){$rendererSession=RmsCheckRendererSession $Receipt $Iterations ([bool]$VulkanHostInstance)}
	$vulkanSession=$null;if($Mode -ceq 'vulkan' -and !$ProtocolReplayHistoricalReceipt){$vulkanSession=RmsCheckVulkanSession $Receipt $Iterations ([bool]$VulkanHostInstance)}
	$seen=@{};$lifetimes=@{};$observationOrdinal=0
	foreach($row in $observations){
		$observationOrdinal++
		$i=RmsField $row 'iteration';RmsRequire ($i -ge 1 -and $i -le $Iterations -and !$seen.ContainsKey([string]$i)) 'Missing or repeated fixture iteration';$seen[[string]$i]=$true
		RmsRequire ((RmsField $row 'scenarioId') -ceq $Scenario -and (RmsField $row 'status') -ceq 'Passed') 'Fixture observation failed or has wrong scenario'
		if($Mode -ceq 'vulkan'){
			RmsCheckVulkanObservation $row $Scenario (!$ProtocolReplayHistoricalReceipt)
			if(!$ProtocolReplayHistoricalReceipt){RmsRequire ($i -eq $observationOrdinal) 'Vulkan context lifetime order differs from iteration order';RmsCheckVulkanRowSession $row $vulkanSession}
		}else{
			RmsCheckRendererObservation $row $Scenario
			RmsRequire ($i -eq $observationOrdinal -and $rendererSession.capturedLifetimeIds[$observationOrdinal-1] -eq $row.captured.lifetimeId) 'Captured lifetime list/order differs from actual observations'
			RmsCheckRendererRowSession $row $rendererSession
			RmsRequire (!$lifetimes.ContainsKey([string]$row.captured.lifetimeId)) 'Independent renderer iterations reused a captured lifetime';$lifetimes[[string]$row.captured.lifetimeId]=$true
		}
	}
	return $observations
}
function RmsRunFixture($Context,[string]$Mode,[string[]]$Scenarios,[int]$Iterations,[switch]$VulkanHostInstance){
	RmsRequire (!$VulkanHostInstance -or ($Mode -cin @('vulkan','renderer') -and $Iterations -ge 100)) 'Host-instance qualification is Vulkan/Renderer-only and requires at least100 independent Engine contexts'
	RmsRequire ($Iterations -ge 100) 'Native acceptance requires at least100 repetitions per lane'
	if($Mode -ceq 'renderer'){RmsRequire ((@($Scenarios|Sort-Object)-join ',') -ceq 'cancelled,completed,failed,restart,scene-clear,shutdown') 'Renderer acceptance requires all six exact lifecycle lanes'}
	if($Mode -ceq 'vulkan'){RmsRequire ((@($Scenarios|Sort-Object)-join ',') -ceq 'acquire-wait-consumed,candidate-rollback,fence-reset-failure,fresh-acquisition,maintenance-disabled,maintenance-enabled,previously-presented,record-failure,recreated-acquisition,submit-failure') 'Vulkan acceptance requires all ten exact native lanes'}
	$rows=New-Object 'Collections.Generic.List[object]'
	foreach($scenario in $Scenarios){
		$caseRoot=Join-Path $Context.work "$Mode-$scenario";$null=[IO.Directory]::CreateDirectory($caseRoot);$receiptPath=Join-Path $caseRoot 'receipt.json';$logs=Join-Path $caseRoot 'process'
		$arguments=@('--mode',$Mode,'--scenario',$scenario,'--iterations',[string]$Iterations,'--install-root',$Context.install.root,'--output',$receiptPath)
		if($VulkanHostInstance){$arguments+=@('--vulkan-host-instance','true')}
		if($Mode -ceq 'renderer'){$arguments+=@('--project',(Join-Path $caseRoot 'project'))}
		& $Context.watchdog -Executable $Context.fixture -ArgumentList $arguments -WorkingDirectory (Split-Path $Context.fixture) -TimeoutSeconds 3600 -LogDirectory $logs|Out-Host;$code=$LASTEXITCODE
		$watchPath=Join-Path $logs 'watchdog.json';$watch=Get-Content $watchPath -Raw|ConvertFrom-Json;RmsRequire (!$watch.timedOut -and $watch.exitCode -ne 124 -and $watch.exitCode -ne 125) 'Fixture watchdog killed or failed launch'
		$receipt=Get-Content $receiptPath -Raw|ConvertFrom-Json
		if($code -eq 2 -and $receipt.status -ceq 'Unverified'){throw "UNVERIFIED: $Mode $scenario unavailable"};RmsRequire ($code -eq 0 -and $watch.exitCode -eq 0) 'Native fixture child failed'
		$actual=RmsCheckFixture $receipt $Mode $scenario $Iterations -VulkanHostInstance:$VulkanHostInstance
		$rows.Add([pscustomobject]@{scenarioId="$Mode-$scenario";status='Passed';observations=$actual;watchdog=$watch;rawEvidence=@((RmsPin $receiptPath),(RmsPin $watchPath),(RmsPin (Join-Path $logs 'stdout.log')),(RmsPin (Join-Path $logs 'stderr.log')))})
	}
	return $rows.ToArray()
}
function RmsRunRenderer($Context,[string[]]$Scenarios,[int]$Iterations,[string]$ToolRoot,[string]$ExistingEditorReceipt,[int]$Port,[switch]$VulkanHostInstance){
	$rows=@(RmsRunFixture $Context renderer $Scenarios $Iterations -VulkanHostInstance:$VulkanHostInstance)
	if($ExistingEditorReceipt){$path=$ExistingEditorReceipt}
	else{
		$helper=RmsInstalled $Context.install 'Tests/Tools/RuntimeMemorySafety/InvokeRuntimeMemorySafetyEditorDiagnostics.ps1'
		$path=Join-Path $Context.work 'editor-loopback.json'
		& $helper -InstallRoot $Context.install.root -ToolInstallRoot $(if($ToolRoot){$ToolRoot}else{$Context.install.root}) -WorkRoot (Join-Path $Context.work 'editor-loopback') -OutputPath $path -McpPort $Port|Out-Host;$code=$LASTEXITCODE
		RmsRequire ($code -eq 0) 'Installed editor loopback failed or was unverified'
	}
	$receipt=Get-Content $path -Raw|ConvertFrom-Json;RmsCheckEditorLoopback $receipt $Context.install.manifest.sha256 $Context.install.root
	$rows+=,[pscustomobject]@{scenarioId='installed-editor-diagnostics';status='Passed';rawEvidence=@((RmsPin $path));observations=$receipt.snapshots}
	return $rows
}
function RmsCheckParity([string]$Path,[string]$RuntimeHash,[string]$FixtureHash){
	$pin=RmsPin $Path;$data=Get-Content $Path -Raw|ConvertFrom-Json
	RmsRequire ((RmsField $data 'schemaVersion') -ceq 'runtime-memory-safety-parity-v1' -and (RmsField $data 'status') -ceq 'Passed' -and (RmsField $data 'runtimeManifestSha256') -ceq $RuntimeHash -and (RmsField $data 'fixtureSha256') -ceq $FixtureHash) 'Parity receipt identity differs from timed candidate'
	$scales=@(RmsField $data 'scales');RmsRequire ($scales.Count -eq 3) 'UNVERIFIED: Missing required parity scales'
	foreach($scale in @('small','normal','stress')){
		$rows=@($scales|Where-Object {$_.scale -ceq $scale});RmsRequire ($rows.Count -eq 1) 'Missing/duplicate parity scale';$r=$rows[0]
		foreach($name in @('cameraLightIdentityExact','cameraLightOrderExact','publishedOutputsEquivalent')){RmsRequire ((RmsField $r $name) -eq $true) "Parity failed: $scale $name"}
		RmsRequire ((RmsField $r 'fallbackCount') -ge 0) 'Parity fallback count missing';$raw=@(RmsField $r 'rawEvidence');RmsRequire ($raw.Count -gt 0) 'UNVERIFIED: Missing raw parity evidence';foreach($p in $raw){RmsCheckPin $p}
	}
	return $pin
}
function RmsRunPerformance($Context,[string]$ReferenceRoot,[string]$CalibrationPath,[string]$ToolRoot,[string]$ParityPath,[int]$Port){
	RmsRequire (![string]::IsNullOrWhiteSpace($ReferenceRoot) -and ![string]::IsNullOrWhiteSpace($CalibrationPath) -and ![string]::IsNullOrWhiteSpace($ToolRoot)) 'UNVERIFIED: Performance requires immutable reference, calibration and ToolInstallRoot'
	$tools=RmsLoadInstall $ToolRoot;$calibrationPin=RmsPin $CalibrationPath;$calibration=Get-Content $CalibrationPath -Raw|ConvertFrom-Json
	RmsRequire ($calibration.status -ceq 'Calibrated' -and @($calibration.reference).Count -eq 18 -and @($calibration.thresholds).Count -eq 6) 'UNVERIFIED: Incomplete sealed reference calibration'
	foreach($record in $calibration.reference){RmsCheckPin $record.pin;foreach($p in $record.dependencies){RmsCheckPin $p};foreach($p in $record.commonTools){RmsRequire (RmsBelow $p.path $tools.root) 'Calibration refers to a different timing tool install'};$runtime=Get-Content $record.metadata.parentEvidence.runtime.path -Raw|ConvertFrom-Json;foreach($p in $runtime.files){RmsRequire (RmsBelow $p.path $ReferenceRoot) 'Calibration refers to a different reference runtime'}}
	RmsRequire (![string]::IsNullOrWhiteSpace($ParityPath)) 'UNVERIFIED: Actual per-scale parity receipt required independently of timing'
	$fixtureHash=(RmsPin (RmsInstalled $Context.install 'Tests/Tools/RuntimeMemorySafety/Fixtures/manifest.json')).sha256
	$parityPin=RmsCheckParity $ParityPath $Context.install.manifest.sha256 $fixtureHash
	$matrix=RmsInstalled $tools 'Tests/Tools/RuntimeMemorySafety/InvokeRuntimeMemorySafetyTimingMatrix.ps1';$output=Join-Path $Context.work 'performance-comparison.json'
	& $matrix -Mode Compare -RuntimeInstallRoot $Context.install.root -ToolInstallRoot $tools.root -CalibrationPath $CalibrationPath -WorkRoot (Join-Path $Context.work 'performance-matrix') -OutputPath $output -McpPort $Port|Out-Host;$code=$LASTEXITCODE
	$result=Get-Content $output -Raw|ConvertFrom-Json;RmsRequire ($code -eq 0 -and $result.status -ceq 'Passed' -and $result.timingPassed -eq $true -and @($result.comparisons).Count -eq 18 -and @($result.comparisons|Where-Object {!$_.passed}).Count -eq 0) 'At least one required per-run timing comparison failed'
	RmsCheckPin $calibrationPin;foreach($p in $tools.pins){RmsCheckPin $p}
	return [pscustomobject]@{scenarioId='performance';status='Passed';comparisonCount=18;rawEvidence=@((RmsPin $output),$calibrationPin,$parityPin);toolManifest=$tools.manifest;comparisons=$result.comparisons}
}
function InvokeRmsInstalledRunner($Configuration,[string]$InvokedPath){
	$hostRequested=($Configuration -is [Collections.IDictionary] -and $Configuration.Contains('VulkanHostInstance') -and [bool]$Configuration.VulkanHostInstance)
	RmsRequire (!$hostRequested -or $Configuration.Mode -cin @('Vulkan','Renderer')) 'VulkanHostInstance must be requested explicitly with Mode Vulkan or Renderer; it never changes timing or other lanes'
	$install=RmsLoadInstall $Configuration.InstallRoot;$work=[IO.Path]::GetFullPath($Configuration.WorkRoot)
	RmsAssertInvocationPaths $install $work $InvokedPath @($Configuration.ToolInstallRoot,$Configuration.ReferenceInstallRoot)
	$registryPath=RmsInstalled $install 'Tests/Tools/RuntimeMemorySafety/ValidationCases.json';$registry=Get-Content $registryPath -Raw|ConvertFrom-Json
	RmsValidateRegistry $registry
	$null=[IO.Directory]::CreateDirectory($work)
	$context=[pscustomobject]@{install=$install;work=$work;registry=$registry;engineTests=(RmsInstalled $install 'Tests/Engine/TerraEngine-EngineTests.exe');editorTests=(RmsInstalled $install 'Tests/Editor/TerraEngine-EditorTests.exe');slice=(RmsInstalled $install 'Tests/Tools/Correctness/RunNativeCorrectnessSlice.ps1');watchdog=(RmsInstalled $install 'Tests/Tools/Correctness/RunWithWatchdog.ps1');fixture=(RmsInstalled $install 'Tests/Tools/RuntimeMemorySafety/RuntimeMemorySafetyFixture.exe')}
	$summary=[ordered]@{schemaVersion='runtime-memory-safety-v1';runId=[Guid]::NewGuid().ToString('N');createdUtc=[DateTime]::UtcNow.ToString('o');mode=$Configuration.Mode;installIdentity=$install.manifest;referenceInstall=$Configuration.ReferenceInstallRoot;toolInstall=$Configuration.ToolInstallRoot;requiredRegistry=(RmsPin $registryPath);observations=@();outcome='Failed';unavailableReasons=@();failures=@()}
	if($hostRequested){$summary.vulkanHostInstance=$true}
	$lanes=$(if($Configuration.Mode -ceq 'All'){@('Cpu','Vulkan','Renderer','Performance')}else{@($Configuration.Mode)})
	foreach($lane in $lanes){try{
		$rows=switch($lane){
			'Cpu'{RmsRunCpu $context $Configuration.Iterations $Configuration.WorkerCounts}
			'Vulkan'{RmsRunFixture $context vulkan @($registry.gpuFailureLanes) $Configuration.Iterations -VulkanHostInstance:$hostRequested}
			'Renderer'{RmsRunRenderer $context @($registry.rendererLifecycleLanes) $Configuration.Iterations $Configuration.ToolInstallRoot $Configuration.EditorDiagnosticsEvidencePath $Configuration.McpPort -VulkanHostInstance:$hostRequested}
			'Performance'{RmsRunPerformance $context $Configuration.ReferenceInstallRoot $Configuration.ReferenceCalibrationPath $Configuration.ToolInstallRoot $Configuration.ParityEvidencePath $Configuration.McpPort}
		};$summary.observations+=@($rows)
	}catch{if($_.Exception.Message.StartsWith('UNVERIFIED:')){$summary.unavailableReasons+=@{lane=$lane;reason=$_.Exception.Message}}else{$summary.failures+=@{lane=$lane;reason=$_.Exception.Message;stack=$_.ScriptStackTrace}}}}
	foreach($p in $install.pins){try{RmsCheckPin $p}catch{$summary.failures+=@{lane='closure';reason=$_.Exception.Message}}}
	$summary.outcome=$(if($summary.failures.Count){'Failed'}elseif($summary.unavailableReasons.Count){'Unverified'}else{'Passed'})
	RmsWrite (Join-Path $work 'summary.json') $summary;return $summary
}
if($MyInvocation.InvocationName -ne '.'){
	Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
	try{$config=@{};foreach($name in @('InstallRoot','WorkRoot','Mode','Iterations','WorkerCounts','ReferenceInstallRoot','ReferenceCalibrationPath','ToolInstallRoot','ParityEvidencePath','EditorDiagnosticsEvidencePath','McpPort','VulkanHostInstance')){$config[$name]=Get-Variable $name -ValueOnly};$result=InvokeRmsInstalledRunner $config $PSCommandPath;Write-Output "Runtime memory safety requested lanes: $($result.outcome)";exit $(if($result.outcome -ceq 'Passed'){0}elseif($result.outcome -ceq 'Unverified'){2}else{1})}catch{Write-Error $_ -ErrorAction Continue;exit 1}
}
