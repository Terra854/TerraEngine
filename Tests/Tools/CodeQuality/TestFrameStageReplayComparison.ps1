<#
Synthetic, process-free protocol regression tests for CompareFrameStageReplay.ps1.
Run with bundled pwsh: ./TestFrameStageReplayComparison.ps1 [-WorkRoot ABS_FRESH].
No engine, editor, Vulkan, collector or runtime process is launched by these tests.
#>
[CmdletBinding()]
param([string]$WorkRoot='', [ValidateRange(263,2147483647)][int]$MinimumTests=263)
$ErrorActionPreference='Stop'
$testRoot=if($WorkRoot){[IO.Path]::GetFullPath($WorkRoot)}else{Join-Path ([IO.Path]::GetTempPath()) ('terra-frame-replay-protocol-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path -LiteralPath $testRoot){throw 'WorkRoot must not already exist.'}
$null=New-Item -ItemType Directory -Path $testRoot
$comparer=Join-Path $PSScriptRoot 'CompareFrameStageReplay.ps1'
. $comparer
$fixturePath=Join-Path $PSScriptRoot 'Fixtures/frame-replay-protocol/synthetic-complete-shape.json'
$manifestSource=Join-Path $PSScriptRoot 'Fixtures/frame-replay-workloads.json'
$manifestFile=Join-Path $testRoot 'manifest.json';Copy-Item -LiteralPath $manifestSource -Destination $manifestFile
$assetFile=Join-Path $testRoot 'assets.json';[IO.File]::WriteAllText($assetFile,'{"synthetic":true}')
$closureFile=Join-Path $testRoot 'closure.json';[IO.File]::WriteAllText($closureFile,'{"synthetic":true,"files":[]}')
$engineFile=Join-Path $testRoot 'mock-engine.dll';[IO.File]::WriteAllText($engineFile,'synthetic engine pin only')
$hostFile=Join-Path $testRoot 'mock-collector.exe';[IO.File]::WriteAllText($hostFile,'synthetic collector pin only')
$projectDirectory=Join-Path $testRoot 'authored-project';$null=New-Item -ItemType Directory -Path $projectDirectory
$testManifest=Get-Content -LiteralPath $manifestFile -Raw|ConvertFrom-Json -AsHashtable
$testResults=[Collections.Generic.List[object]]::new()
function FstPin([string]$Path){return @{path=[IO.Path]::GetFullPath($Path);sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $Path).Length}}
function FstClone($Value){return ($Value|ConvertTo-Json -Depth 100 -Compress|ConvertFrom-Json -AsHashtable)}
function FstWrite([string]$Path,$Value){[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 100),[Text.UTF8Encoding]::new($false))}
# Synthetic metadata follows the native three-publication camera loop. Both
# camera payloads intentionally coincide while their authored UUIDs differ.
function FstMultiOutput($MultiLane) {
	$matrix=@(for($column=0;$column -lt 4;$column++){,@(for($row=0;$row -lt 4;$row++){@{ieee754=$(if($column -eq $row){1065353216}else{0})}})})
	$words=@(for($column=0;$column -lt 4;$column++){for($row=0;$row -lt 4;$row++){if($column -eq $row){1065353216}else{0}}})
	$pose=@{cell=@(0,0,0);offsetBits=@(0,0,0);rotationBits=@(0,0,0,1065353216);scaleBits=@(1065353216,1065353216,1065353216);transformRevision=1;coordinateFrameRevision=1}
	$cameraIds=@('068c0000000000000000000000000001','068c0000000000000000000000000002')
	$MultiLane.authoritative.sceneViewControl=@{authoredCameraId=$cameraIds[1];canonicalPose=$pose;viewMatrixBits=$words;projectionMatrixBits=$words;targetExtent=@(1280,720);extentSource='WindowSystem.GetLatestWorkerSnapshot';runtimePrimaryOverride=$false}
	foreach($multiFrame in $MultiLane.authoritative.frames) {
		$cameras=@(foreach($cameraId in $cameraIds){@{objectId=$cameraId;verticalFovBits=1114636288;aspectRatioBits=1071877689;nearPlaneBits=1036831949;farPlaneBits=1132068864;upBits=@(0,1065353216,0)}})
		$multiFrame.capturedInputs=@{frameId=$multiFrame.renderer.root.frameId;cameras=$cameras;lights=@()}
		$multiFrame.renderer.root.views=@(for($index=0;$index -lt 3;$index++){@{outputKind=$(if($index -eq 0){1}else{2});viewType=$(if($index -eq 0){1}else{0});outputId=$(if($index -eq 0){1}else{2});generation=$multiFrame.renderer.root.generation;lightIdentity=0;visibleOrdinals=@();selectedLods=@();routeSelectors=@();packetTemplateIdentities=@();tintSelectors=@();resolvedCommands=@()}})
		$multiFrame.renderer.root.presentationInputs=@(for($index=0;$index -lt 3;$index++){@{output=@{kind=$(if($index -eq 0){1}else{2});diagnosticsView=$(if($index -eq 0){1}else{0});outputId=$(if($index -eq 0){1}else{2});priority=0;targetSize=$(if($index -eq 0){@(1280,720)}else{@(0,0)})};viewMatrix=$matrix;projectionMatrix=$matrix}})
		$multiFrame.renderer.root.submissionRanges=@(for($index=0;$index -lt 3;$index++){@{route=0;viewIndex=$index;firstSelection=0;selectionCount=1;packetTemplateIdentity=1;singleton=$true}})
		$multiFrame.multiOutputObservation=@{association='CapturedCameraOrderToNativeRuntimeSubmissionOrder';expectedAuthoredCameraIds=$cameraIds;scenePresentationIndex=0;rangeViewIndices=@(0,1,2);passed=$true;cameraObservations=@(for($ordinal=0;$ordinal -lt 2;$ordinal++){@{cameraOrdinal=$ordinal;presentationIndex=($ordinal+1);capturedCamera=(FstClone $cameras[$ordinal]);protectedCamera=(FstClone $cameras[$ordinal]);canonicalPose=(FstClone $pose);cameraEnabled=$true;activeTransformCamera=$true;derivedViewMatrix=$matrix;derivedProjectionMatrix=$matrix}})}
	}
}
function FstCase([string]$Name,[scriptblock]$Mutation={},[string]$Reject='', [string]$Scenario='cold-direct-complete',[int]$Workers=1) {
	$caseDirectory=Join-Path $testRoot $Name;$null=New-Item -ItemType Directory -Path $caseDirectory
	$reference=Get-Content -LiteralPath $fixturePath -Raw|ConvertFrom-Json -AsHashtable
	$scenarioObject=@($testManifest.scenarios|Where-Object {$_.id -ceq $Scenario})[0]
	$reference.scenarioId=$Scenario;$reference.requestedWorkers=$Workers;$reference.actualWorkers=$Workers
	$reference.controls=@{manifest=(FstPin $manifestFile);assetManifest=(FstPin $assetFile);projectClosure=(FstPin $closureFile);projectRoot=$projectDirectory;loadedEngine=(FstPin $engineFile);hostExecutable=(FstPin $hostFile);scenario=(FstClone $scenarioObject);expectedEnginePath=$engineFile;expectedEngineSha256=(FstPin $engineFile).sha256;engineHashVerified=$true;resolvedAssetManifests=@((FstPin $assetFile))}
	foreach($lane in $reference.lanes) {
		$lane.authoritative.scenarioAssertions=@(foreach($id in (FsrRequiredAssertions $Scenario $lane.laneId)){$v=if($id -ceq 'actual-workers'){$Workers}elseif($id -ceq 'observed-frame-count'){3}else{$true};@{id=$id;expected=$v;observed=$v;passed=$true}})
		if($Scenario -cin @('cold-direct-complete','cold-virtual-compact')){$lane.authoritative.frames[0].renderer.root.receipt.reusedPersistentPages=0}
		if($Scenario -ceq 'worker-failure' -and $lane.laneId -ceq 'ProducerBoundaries'){
			$plan=$lane.authoritative.frames[0].scheduler;$plan.valid=$false;$plan.completed=$false
			$plan.firstFailure=@{code=8;workloadName='GeometryRoutePrepare';resourceName='RenderSystem/Render/GeometryRoutePrepare#0';message='Job threw during worker execution: frame-replay-real-workload-fault.'}
			foreach($declaration in $plan.declarations){if($declaration.workloadName -ceq 'GeometryRoutePrepare'){$declaration.workloadName='PublishAnimationAuthority'}}
			$plan.declarations[0].workloadName='GeometryRoutePrepare';$plan.declarations[0].stableDescriptorId='RenderSystem/Render/GeometryRoutePrepare';$plan.results[0].stableDescriptorId='RenderSystem/Render/GeometryRoutePrepare';$plan.results[0].completed=$false;$plan.results[0].executionPolicy=1;$plan.results[0].inlineSelected=$false;$plan.results[0].fallbackUsed=$false;$plan.declarations[0].workerEligible=$true;$plan.declarations[0].mainThreadRequired=$false;$plan.declarations[0].estimatedWorkItemCount=19;$plan.declarations[0].inlineWorkItemThreshold=16
			foreach($event in @($lane.diagnostic.runtimeIdentifiers[0].events[0],$lane.diagnostic.runtimeIdentifiers[0].events[1],$lane.authoritative.boundaries[0],$lane.authoritative.boundaries[1])){$event.stableDescriptorId='RenderSystem/Render/GeometryRoutePrepare'}
			$lane.authoritative.causality[0].expectedCompletedInvocationCount=1;$lane.diagnostic.runtimeIdentifiers[0].events[1].failed=$true;$lane.authoritative.boundaries[1].failed=$true
			for($admissionIndex=0;$admissionIndex -lt 3;$admissionIndex++){$complete=if($admissionIndex -eq 0){1}else{2};$compact=if($admissionIndex -eq 0){1}else{0};$lane.authoritative.frames[$admissionIndex].animationAdmission=@{planStatus=0;rejectionReason=0;rejectionItemIndex=0;executionPolicy=0;availableWorkerCount=$Workers;selectedWorkerCount=$Workers;itemCount=2;rangeCount=1;scratchBytes=0;cacheEntryCount=1;reusedItemCount=0;fallbackItemCount=0;deterministicPublicationValidated=$true;cancellationRequested=$false;counters=@{activeAnimators=2;compactEligibleAnimators=$compact;compactProcessedAnimators=$compact;completeCapturedAnimators=$complete;completeProcessedAnimators=$complete}}}
			$lane.authoritative.frames[0].renderer.root.generation=16;$lane.authoritative.frames[0].renderer.root.receipt.publicationGeneration=0
			$lane.authoritative.profileObservation=@{requestedProfile='Mixed';checkpoint='FaultFrameBeforeRendererClear';frameOrdinal=0;nativePublicationFrameId=$lane.authoritative.frames[0].renderer.root.frameId;nativePublicationGeneration=$lane.authoritative.frames[0].renderer.root.generation;nativePublicationReceiptGeneration=$lane.authoritative.frames[0].renderer.root.receipt.publicationGeneration;directCommands=1;virtualCommands=1;completeProcessedAnimators=1;compactProcessedAnimators=1}
			$lane.authoritative.postRetirementProfileObservation=@{checkpoint='AfterMeasuredRecoveryFrames';frameOrdinal=2;qualifying=$false;animationAdmission=(FstClone $lane.authoritative.frames[2].animationAdmission)}
		}
		if($Scenario -ceq 'multi-output'){FstMultiOutput $lane}
		if($Scenario -ceq 'no-output'){foreach($frame in $lane.authoritative.frames){$frame.renderer=@{present=$false;root=$null};$frame.outputObservation=@{diagnosticsFrameId=1;diagnosticsValid=$true;diagnosticsPublicationGeneration=0;viewSubmitWorkCount=0;gameStatsValid=$false;sceneStatsValid=$false;scratchCompleted=$true};$frame.capturedInputs=@{frameId=1;cameras=@();lights=@()};$frame.capturedRenderables=@();$frame.sceneMeshInputs=@(@{objectId='authored-object';meshId='authored-mesh';materialId='authored-material';spatialObjectId='authored-spatial';taskShaderCullingEnabled=$false})}}
		if($Scenario -cin @('cancel-before-exchange','cancel-after-exchange') -and $lane.laneId -ceq 'ProducerBoundaries'){
			$first=$lane.authoritative.frames[0].commandReceipts[0];$first.status=if($Scenario -ceq 'cancel-before-exchange'){13}else{0};$first.publicationGeneration=if($Scenario -ceq 'cancel-before-exchange'){0}else{$lane.authoritative.frames[0].animation.root.authorityGeneration}
			$lane.authoritative.cancellationObservation=@{firstCommandReceipt=(FstClone $first);cacheCounts=@{entriesBeforeIngress=1;entriesAfterIngress=1;entriesAfterCallbacks=2};cacheMembershipClaim='CountOnlyNoEntryIdentityProof'}
		}
		if($Scenario -ceq 'restart') {
			$lane.authoritative.restartRetainedRoots=@{heldRenderImmutable=$true;heldAnimationImmutable=$true;preShutdownRenderImmutable=$true;preShutdownAnimationImmutable=$true}
			$lane.authoritative.restartShutdownObservation=@{rendererRootRetired=$true;animationRootRetired=$true;contextReleased=$true}
			# Equal renderer counters are intentional: allocation identity is observed
			# separately while the old strong publication remains retained.
			$lane.authoritative.restartPublications=@{renderer=(FstClone $lane.authoritative.frames[2].renderer);animation=(FstClone $lane.authoritative.frames[2].animation)}
			$lane.authoritative.restartPublications.animation.root.sessionGeneration++
			$old=@{};$fresh=@{}
			foreach($identityName in @('rendererSessionGeneration','backendResourceGeneration','generation','frameId')){$old[$identityName]=$lane.authoritative.frames[2].renderer.root[$identityName];$fresh[$identityName]=$lane.authoritative.restartPublications.renderer.root[$identityName]}
			$lane.authoritative.restartPublicationIdentity=@{rendererGenerationDomain='BackendRecoveryInstanceMayRepeatAcrossReinitialization';old=$old;fresh=$fresh;freshContextPresent=$true;distinctRetainedPublication=$true}
		}

		if($Scenario -cin @('held-reader','scene-clear','shutdown','restart')){$lane.authoritative.lifetimeObservations=@(@{checkpoint='SyntheticHeldRoot';immutable=$true;generation=1})}
	}
	$candidate=FstClone $reference;$candidate.runNonce='synthetic-candidate'
	$referenceFile=Join-Path $caseDirectory 'reference.json';$candidateFile=Join-Path $caseDirectory 'candidate.json';$joinFile=Join-Path $caseDirectory 'provenance.json'
	$trusted=@{schemaVersion='frame-stage-replay-provenance-v1';protocolFixture=$true;reference=@{runNonce=$reference.runNonce;loadedEngine=(FstPin $engineFile);hostExecutable=(FstPin $hostFile);projectRoot=$projectDirectory};candidate=@{runNonce=$candidate.runNonce;loadedEngine=(FstPin $engineFile);hostExecutable=(FstPin $hostFile);projectRoot=$projectDirectory};controlPathPairs=@(foreach($kind in @('manifest','assetManifest','projectClosure')){@{kind=$kind;reference=$reference.controls[$kind].path;candidate=$candidate.controls[$kind].path}})+@(@{kind='projectRoot';reference=$projectDirectory;candidate=$projectDirectory})}
	$context=@{r=$reference;c=$candidate;j=$trusted;map=$null;rawCandidate=$null;protocolOnly=$true;directory=$caseDirectory}
	& $Mutation $context
	FstWrite $referenceFile $context.r
	if($null -ne $context.rawCandidate){[IO.File]::WriteAllText($candidateFile,$context.rawCandidate)}else{FstWrite $candidateFile $context.c}
	$context.j.reference.receipt=FstPin $referenceFile;$context.j.candidate.receipt=FstPin $candidateFile
	FstWrite $joinFile $context.j
	$mapFile='';if($null -ne $context.map){$mapFile=Join-Path $caseDirectory 'session-map.json';FstWrite $mapFile $context.map}
	$observedError='';$result=$null
	try{$result=InvokeFrameStageReplayComparison -ReferencePath $referenceFile -CandidatePath $candidateFile -ManifestPath $manifestFile -ProvenanceJoinPath $joinFile -SessionMapPath $mapFile -ProtocolOnly:$context.protocolOnly}catch{$observedError=$_.Exception.Message}
	if($Reject){if(!$observedError.Contains("[FrameReplay:$Reject]")){throw "$Name expected rejection $Reject, observed: $observedError"}}
	else{if($observedError){throw "$Name unexpectedly rejected: $observedError"};if($result.status -cne 'Passed' -or $result.replayAcceptance -or $result.gpuAcceptance -or $result.realProcessesLaunched){throw "$Name falsely claimed acceptance"}}
	$testResults.Add([pscustomobject]@{name=$Name;passed=$true;expectedRejection=$Reject})
}
FstCase 'synthetic-equal'
FstCase 'diagnostic-only-change' {param($x)$x.c.lanes[0].diagnostic.timings=@(999);$x.c.lanes[1].diagnostic.runtimeIdentifiers+=@('different-address')}
foreach($scenario in $testManifest.scenarios){foreach($workers in @(1,2,8)){FstCase ('scenario-'+$scenario.id+'-workers-'+$workers) {} '' $scenario.id $workers}}
FstCase 'wrong-schema' {param($x)$x.c.schemaVersion='frame-stage-replay-v2'} 'schema'
FstCase 'real-mode-rejects-synthetic' {param($x)$x.protocolOnly=$false;$x.j.protocolFixture=$false} 'boolean'
FstCase 'synthetic-join-rejected-real-mode' {param($x)$x.protocolOnly=$false} 'boolean'
FstCase 'incomplete-execution' {param($x)$x.c.status='Incomplete'} 'incomplete'
FstCase 'failed-execution' {param($x)$x.c.status='Failed'} 'incomplete'
FstCase 'unsupported-observation' {param($x)$x.c.unsupportedObservations=@('Unavailable')} 'unsupported'
FstCase 'zero-actual-workers' {param($x)$x.c.actualWorkers=0} 'integer'
FstCase 'wrong-actual-workers' {param($x)$x.c.actualWorkers=2} 'workers'
FstCase 'unsupported-worker-count' {param($x)$x.c.actualWorkers=3;$x.c.requestedWorkers=3} 'workers'
FstCase 'worker-count-pair-drift' {param($x)$x.c.actualWorkers=2;$x.c.requestedWorkers=2;foreach($l in $x.c.lanes){$l.authoritative.scenarioAssertions[0].expected=2;$l.authoritative.scenarioAssertions[0].observed=2}} 'workers'
FstCase 'unknown-scenario' {param($x)$x.r.scenarioId='not-real'} 'scenario'
FstCase 'scenario-id-drift' {param($x)$x.c.scenarioId='held-reader'} 'scenario'
FstCase 'missing-lane' {param($x)$x.c.lanes=@($x.c.lanes[0])} 'lane'
FstCase 'duplicate-lane' {param($x)$x.c.lanes[1]=FstClone $x.c.lanes[0]} 'lane'
FstCase 'lane-order-drift' {param($x)$x.c.lanes=@($x.c.lanes[1],$x.c.lanes[0])} 'lane'
FstCase 'cross-lane-substitution' {param($x)$x.c.lanes[1].authoritative=FstClone $x.c.lanes[0].authoritative} 'lane'
FstCase 'coverage-lane-gap' {param($x)$x.c.coverageJoin.observedLanes=@('FullEngine')} 'difference'
FstCase 'lane-incomplete' {param($x)$x.c.lanes[0].status='Incomplete'} 'incomplete'
FstCase 'empty-measured-frames' {param($x)$x.c.lanes[0].authoritative.frames=@()} 'empty-proof'
FstCase 'duplicate-frame-ordinal' {param($x)$x.c.lanes[0].authoritative.frames[1].ordinal=0} 'duplicate-id'
FstCase 'frame-order-drift' {param($x)$f=$x.c.lanes[0].authoritative.frames;$x.c.lanes[0].authoritative.frames=@($f[1],$f[0],$f[2])} 'difference' 'animated-mixed'
FstCase 'missing-root-field' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root.Remove('resourcePreparation')} 'field'
FstCase 'count-only-projection' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root=@{count=3}} 'field'
FstCase 'empty-all-root-payloads' {param($x)foreach($f in $x.c.lanes[0].authoritative.frames){$f.renderer.root.persistent.pages=@();$f.animation.root.pages=@()}} 'empty-proof'
FstCase 'authored-object-id-change' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root.persistent.pages[0].slots[0].objectId=2} 'difference'
FstCase 'authored-animation-id-change' {param($x)$x.c.lanes[0].authoritative.frames[0].animation.root.pages[0].rows[0].ownerObjectId=2} 'difference'
FstCase 'duplicate-render-slot' {param($x)$p=$x.c.lanes[0].authoritative.frames[0].renderer.root.persistent.pages[0];$p.slots+=@(FstClone $p.slots[0])} 'duplicate-id'
FstCase 'duplicate-animation-row' {param($x)$p=$x.c.lanes[0].authoritative.frames[0].animation.root.pages[0];$p.rows+=@(FstClone $p.rows[0])} 'duplicate-id'
FstCase 'resource-generation-change' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root.persistent.pages[0].slots[0].resourceGeneration=2} 'difference'
FstCase 'lifecycle-generation-change' {param($x)$x.c.lanes[0].authoritative.frames[0].animation.root.lifecycleGeneration=2} 'difference'
FstCase 'additional-authoritative-field' {param($x)$x.c.lanes[0].authoritative.extra=$true} 'difference'
FstCase 'changed-unknown-authoritative-field' {param($x)$x.r.lanes[0].authoritative.extra=1;$x.c.lanes[0].authoritative.extra=2} 'difference'
FstCase 'phase-order-drift' {param($x)$x.r.lanes[0].authoritative.frames[0].scheduler.phaseNames=@('A','B');$x.c.lanes[0].authoritative.frames[0].scheduler.phaseNames=@('B','A')} 'difference'
FstCase 'flush-change' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.declaredFlushNames=@('OtherBoundary')} 'difference'
FstCase 'resource-declaration-change' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.declarations[0].reads=@('DifferentInput')} 'difference'
FstCase 'duplicate-descriptor-id' {param($x)$s=$x.c.lanes[0].authoritative.frames[0].scheduler;$s.declarations+=@(FstClone $s.declarations[0])} 'duplicate-id'
FstCase 'unknown-result-descriptor' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.results[0].stableDescriptorId='unknown'} 'duplicate-id'
FstCase 'missing-scheduler-results' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.results=@()} 'empty-proof'
FstCase 'empty-producer-boundaries' {param($x)$x.c.lanes[1].authoritative.boundaries=@()} 'empty-proof'
FstCase 'boundary-change' {param($x)$x.c.lanes[1].authoritative.boundaries[0].checkpoint='Changed'} 'difference'
FstCase 'mutation-observation-change' {param($x)$x.c.lanes[0].authoritative.mutationObservations[0].after=$false} 'difference'
FstCase 'missing-mutation-observations' {param($x)$x.c.lanes[0].authoritative.mutationObservations=@()} 'empty-proof'
FstCase 'missing-command-receipts' {param($x)foreach($f in $x.c.lanes[0].authoritative.frames){$f.commandReceipts=@()}} 'empty-proof'
FstCase 'count-only-command-receipt' {param($x)$x.c.lanes[0].authoritative.frames[0].commandReceipts=@(@{})} 'field'
FstCase 'command-receipt-change' {param($x)$x.c.lanes[0].authoritative.frames[0].commandReceipts[0].publicationGeneration=2} 'difference'
FstCase 'no-op-assertion' {param($x)$a=$x.c.lanes[0].authoritative.scenarioAssertions[2];$a.expected=$false;$a.observed=$false} 'boolean'
FstCase 'failed-assertion' {param($x)$x.c.lanes[0].authoritative.scenarioAssertions[2].passed=$false} 'boolean'
FstCase 'assertion-observation-mismatch' {param($x)$x.c.lanes[0].authoritative.scenarioAssertions[2].observed=$false} 'difference'
FstCase 'missing-required-predicate' {param($x)$x.c.lanes[0].authoritative.scenarioAssertions=@($x.c.lanes[0].authoritative.scenarioAssertions|Where-Object {$_.id -cne 'authored-render-identity'})} 'assertion'
FstCase 'duplicate-assertion-id' {param($x)$x.c.lanes[0].authoritative.scenarioAssertions+=@(FstClone $x.c.lanes[0].authoritative.scenarioAssertions[0])} 'duplicate-id'
FstCase 'missing-lifecycle-observation' {param($x)$x.c.lanes[0].authoritative.lifetimeObservations=@()} 'empty-proof' 'held-reader'
FstCase 'held-root-mutated' {param($x)$x.c.lanes[0].authoritative.lifetimeObservations[0].generation=2} 'difference' 'held-reader'
FstCase 'scenario-control-drift' {param($x)$x.c.controls.scenario.warmupFrames=999} 'difference'
FstCase 'nonce-forgery' {param($x)$x.c.runNonce='forged'} 'difference'
FstCase 'runtime-pin-forgery' {param($x)$x.c.controls.loadedEngine.sha256=('0'*64)} 'pin'
FstCase 'collector-pin-forgery' {param($x)$x.j.candidate.hostExecutable.sha256=('0'*64)} 'pin'
FstCase 'unreviewed-control-path' {param($x)$x.j.controlPathPairs=@($x.j.controlPathPairs|Where-Object {$_.kind -cne 'projectClosure'})} 'provenance'
FstCase 'duplicate-control-path-pair' {param($x)$x.j.controlPathPairs+=@(FstClone $x.j.controlPathPairs[0])} 'provenance'
FstCase 'authored-closure-content-drift' {param($x)$p=Join-Path $x.directory 'different-closure.json';[IO.File]::WriteAllText($p,'changed');$x.c.controls.projectClosure=FstPin $p} 'control'
FstCase 'asset-manifest-content-drift' {param($x)$p=Join-Path $x.directory 'different-assets.json';[IO.File]::WriteAllText($p,'changed');$x.c.controls.assetManifest=FstPin $p} 'control'
FstCase 'join-schema-drift' {param($x)$x.j.schemaVersion='invented'} 'provenance'
FstCase 'duplicate-json-property' {param($x)$x.rawCandidate='{"schemaVersion":"frame-stage-replay-v1","schemaVersion":"forged"}'} 'duplicate-key'
FstCase 'unmapped-session-drift' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root.rendererSessionGeneration=9} 'difference'
FstCase 'explicit-bijective-session-map' {param($x)foreach($f in $x.c.lanes[0].authoritative.frames){$f.renderer.root.rendererSessionGeneration=9;$f.renderer.root.persistent.rendererSessionGeneration=9};$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Renderer';reference='1';candidate='9'})}}
FstCase 'inconsistent-session-map' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root.rendererSessionGeneration=9;$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Renderer';reference='1';candidate='9'})}} 'session-map'
FstCase 'non-bijective-session-map' {param($x)$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Renderer';reference='1';candidate='9'},@{laneId='FullEngine';domain='Renderer';reference='2';candidate='9'})}} 'session-map'
FstCase 'unused-session-map' {param($x)$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Renderer';reference='55';candidate='9'})}} 'session-map'
FstCase 'session-map-no-generic-key-rewrite' {param($x)foreach($f in $x.c.lanes[0].authoritative.frames){$f.animation.root.sessionGeneration=9};$x.r.lanes[0].authoritative.unreviewed=@{sessionGeneration=1};$x.c.lanes[0].authoritative.unreviewed=@{sessionGeneration=9};$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Animation';reference='1';candidate='9'})}} 'difference'
FstCase 'exact-large-uint64-difference' {param($x)$x.r.lanes[0].authoritative.exact=[uint64]::MaxValue;$x.c.lanes[0].authoritative.exact=[uint64]::MaxValue-1} 'difference'
FstCase 'numeric-type-difference' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root.generation='1'} 'difference'
FstCase 'float-bit-pattern-difference' {param($x)$x.r.lanes[0].authoritative.poseBits=@(2147483648);$x.c.lanes[0].authoritative.poseBits=@(0)} 'difference'
FstCase 'raw-causality-missing' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers=@()} 'causality'
FstCase 'raw-causality-duplicate-frame' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers+=@(FstClone $x.c.lanes[1].diagnostic.runtimeIdentifiers[0])} 'causality'
FstCase 'raw-causality-reversed-sequence' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[1].observedSequence=1} 'causality'
FstCase 'raw-causality-unknown-descriptor' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[0].stableDescriptorId='forged'} 'causality'
FstCase 'raw-causality-missing-exit' {param($x)$v=$x.c.lanes[1].diagnostic.runtimeIdentifiers[0];$v.events=@($v.events[0],$v.events[2],$v.events[3]);for($i=0;$i -lt 3;$i++){$v.events[$i].observedSequence=$i+1}} 'causality'
FstCase 'raw-causality-partition-mismatch' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[1].itemEnd=2} 'causality'
FstCase 'raw-causality-before-prerequisite' {param($x)$v=$x.c.lanes[1].diagnostic.runtimeIdentifiers[0];$v.events=@($v.events[2],$v.events[3],$v.events[0],$v.events[1]);for($i=0;$i -lt 4;$i++){$v.events[$i].observedSequence=$i+1}} 'causality'
FstCase 'raw-causality-completed-not-observed' {param($x)$v=$x.c.lanes[1].diagnostic.runtimeIdentifiers[0];$v.events=@($v.events[0],$v.events[1])} 'causality'
FstCase 'raw-causality-summary-forged' {param($x)$x.c.lanes[1].authoritative.causality[0].observedInvocationCount=999} 'causality'
FstCase 'raw-causality-claimed-failed' {param($x)$x.c.lanes[1].authoritative.causality[0].passed=$false} 'causality'
FstCase 'raw-causality-wrong-execution-order' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[0].executionOrder=999} 'causality'
FstCase 'raw-causality-invalid-dependency' {param($x)$x.c.lanes[1].authoritative.frames[0].scheduler.dependencyEdges=@(@(0,999))} 'causality'
FstCase 'raw-causality-missing-summary' {param($x)$x.c.lanes[1].authoritative.Remove('causality')} 'causality'
FstCase 'object-property-order-is-not-array-order' {param($x)$x.c.lanes[0].authoritative.mutationObservations[0]=[ordered]@{after=$true;before=$false;checkpoint='SyntheticOnly'}}
FstCase 'scheduler-jobgraph-extra-change' {param($x)$x.r.lanes[0].authoritative.frames[0].scheduler.jobGraph.extra='A';$x.c.lanes[0].authoritative.frames[0].scheduler.jobGraph.extra='B'} 'difference'
FstCase 'runtime-other-file-same-bytes' {param($x)$p=Join-Path $x.directory 'other-engine.dll';Copy-Item -LiteralPath $x.c.controls.loadedEngine.path -Destination $p;$x.c.controls.loadedEngine=FstPin $p} 'provenance'
FstCase 'manifest-pin-byte-forgery' {param($x)$x.c.controls.manifest.bytes++} 'pin'
FstCase 'session-map-lane-scope' {param($x)foreach($f in $x.c.lanes[1].authoritative.frames){$f.renderer.root.rendererSessionGeneration=9};$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Renderer';reference='1';candidate='1'})}} 'difference'
FstCase 'session-map-lifecycle-never-normalized' {param($x)foreach($f in $x.c.lanes[0].authoritative.frames){$f.animation.root.sessionGeneration=9};$x.c.lanes[0].authoritative.frames[0].animation.root.lifecycleGeneration=9;$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Animation';reference='1';candidate='9'})}} 'difference'
FstCase 'raw-valid-independent-sibling-reordering' {param($x)
	foreach($receipt in @($x.r,$x.c)){foreach($frame in $receipt.lanes[1].authoritative.frames){$frame.scheduler.dependencyEdges=@()};foreach($check in $receipt.lanes[1].authoritative.causality){$check.checkedDependencyCount=0}}
	$v=$x.c.lanes[1].diagnostic.runtimeIdentifiers[0];$v.events=@($v.events[2],$v.events[3],$v.events[0],$v.events[1]);for($i=0;$i -lt 4;$i++){$v.events[$i].observedSequence=$i+1}
}
FstCase 'raw-failed-probe-pair-remains-mandatory' {param($x)
	foreach($receipt in @($x.r,$x.c)){$l=$receipt.lanes[1];$l.authoritative.frames[0].scheduler.results[1].completed=$false;$l.authoritative.causality[0].expectedCompletedInvocationCount=1;$l.diagnostic.runtimeIdentifiers[0].events[3].failed=$true;$l.authoritative.boundaries[3].failed=$true}
}
FstCase 'raw-dependent-missing-prerequisite' {param($x)$l=$x.c.lanes[1];$l.authoritative.frames[0].scheduler.results[0].completed=$false;$v=$l.diagnostic.runtimeIdentifiers[0];$v.events=@($v.events[2],$v.events[3]);$v.events[0].observedSequence=1;$v.events[1].observedSequence=2} 'causality'
FstCase 'raw-zero-partition-count' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[0].partitionCount=0} 'causality'
FstCase 'raw-canonical-boundary-forged' {param($x)$x.c.lanes[1].authoritative.boundaries[0].itemEnd=999} 'difference'
FstCase 'raw-descriptor-case-forgery' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[0].stableDescriptorId='SYNTHETIC.DESCRIPTOR'} 'causality'
FstCase 'raw-exit-without-enter' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[0].checkpoint='Exit'} 'causality'
foreach($faultScenario in @('rejection-rollback','cancel-before-exchange','cancel-after-exchange','worker-failure')) {
	FstCase ('missing-producer-predicate-'+$faultScenario) {param($x)$x.c.lanes[1].authoritative.scenarioAssertions=@($x.c.lanes[1].authoritative.scenarioAssertions|Select-Object -SkipLast 1)} 'assertion' $faultScenario
}
FstCase 'engine-hash-not-verified' {param($x)$x.c.controls.engineHashVerified=$false} 'boolean'
FstCase 'engine-expected-hash-forged' {param($x)$x.c.controls.expectedEngineSha256=('0'*64)} 'provenance'
FstCase 'renderer-null-outside-no-output' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer=@{present=$false;root=$null}} 'boolean' 'animated-mixed'
FstCase 'animation-null-is-not-authority' {param($x)$x.c.lanes[0].authoritative.frames[0].animation=@{present=$false;root=$null}} 'boolean'
FstCase 'no-output-missing-diagnostics' {param($x)$x.c.lanes[0].authoritative.frames[0].Remove('outputObservation')} 'field' 'no-output'
FstCase 'no-output-live-publication' {param($x)$x.c.lanes[0].authoritative.frames[0].outputObservation.diagnosticsPublicationGeneration=1} 'no-output' 'no-output'
FstCase 'no-output-injected-camera' {param($x)$x.c.lanes[0].authoritative.frames[0].capturedInputs.cameras=@(@{objectId='camera'})} 'no-output' 'no-output'
FstCase 'no-output-unfinished-scratch' {param($x)$x.c.lanes[0].authoritative.frames[0].outputObservation.scratchCompleted=$false} 'boolean' 'no-output'
FstCase 'no-output-missing-authored-inputs' {param($x)$x.c.lanes[0].authoritative.frames[0].sceneMeshInputs=@()} 'no-output' 'no-output'
FstCase 'no-output-empty-authored-id' {param($x)$x.c.lanes[0].authoritative.frames[0].sceneMeshInputs[0].meshId='00000000'} 'no-output' 'no-output'
FstCase 'expected-engine-path-forgery' {param($x)$x.c.controls.expectedEnginePath=$x.c.controls.hostExecutable.path} 'provenance'
FstCase 'expected-engine-path-separators' {param($x)$x.c.controls.expectedEnginePath=$x.c.controls.expectedEnginePath.Replace('\','/')}
FstCase 'session-map-slash-property-never-normalized' {param($x)foreach($f in $x.c.lanes[0].authoritative.frames){$f.renderer.root.rendererSessionGeneration=9;$f.renderer.root.persistent.rendererSessionGeneration=9};$x.r.lanes[0].authoritative['frames/0/renderer/root/rendererSessionGeneration']=1;$x.c.lanes[0].authoritative['frames/0/renderer/root/rendererSessionGeneration']=9;$x.map=@{schemaVersion='frame-stage-session-map-v1';mappings=@(@{laneId='FullEngine';domain='Renderer';reference='1';candidate='9'})}} 'difference'
FstCase 'resolved-assets-missing' {param($x)$x.c.controls.Remove('resolvedAssetManifests')} 'field'
FstCase 'resolved-assets-empty' {param($x)$x.c.controls.resolvedAssetManifests=@()} 'control'
FstCase 'resolved-assets-duplicate' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.controls.resolvedAssetManifests+=@(FstClone $receipt.controls.resolvedAssetManifests[0])}} 'control'
FstCase 'resolved-assets-first-not-main' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.controls.resolvedAssetManifests=@(FstClone $receipt.controls.projectClosure)}} 'provenance'
FstCase 'resolved-assets-content-drift' {param($x)$p=Join-Path $x.directory 'resolved-other.json';[IO.File]::WriteAllText($p,'different');$x.c.controls.resolvedAssetManifests[0]=FstPin $p} 'control'
FstCase 'raw-coordinator-fact-missing' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[0].Remove('onCoordinator')} 'causality'
FstCase 'worker-fault-on-coordinator-rejected' {param($x)foreach($event in $x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events){$event.onCoordinator=$true}} 'causality' 'worker-failure'
FstCase 'worker-eligibility-without-actual-fault-rejected' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[1].failed=$false} 'causality' 'worker-failure'
FstCase 'full-engine-jobgraph-missing' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.Remove('jobGraph')} 'field'
FstCase 'full-engine-jobgraph-count-only' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.jobGraph=@{workerJobCount=1}} 'field'
FstCase 'producer-jobgraph-cross-lane-claim' {param($x)$x.c.lanes[1].authoritative.frames[0].scheduler.jobGraph=FstClone $x.c.lanes[0].authoritative.frames[0].scheduler.jobGraph} 'lane'
FstCase 'raw-callback-thread-switch-forged' {param($x)$x.c.lanes[1].diagnostic.runtimeIdentifiers[0].events[1].onCoordinator=$true} 'causality'
FstCase 'resolved-assets-reviewed-project-root-mapping' {param($x)
	foreach($side in @('r','c')){$root=Join-Path $x.directory $side;$dir=Join-Path $root 'Build/Assets/.terra-animation-manifests';$null=New-Item -ItemType Directory -Path $dir -Force;$p=Join-Path $dir 'generated.json';[IO.File]::WriteAllText($p,'same immutable input');$x[$side].controls.projectRoot=$root;$x[$side].controls.resolvedAssetManifests+=@((FstPin $p));$trustedSide=if($side -ceq 'r'){'reference'}else{'candidate'};$x.j[$trustedSide].projectRoot=$root}
	$pair=@($x.j.controlPathPairs|Where-Object kind -CEQ 'projectRoot')[0];$pair.reference=$x.r.controls.projectRoot;$pair.candidate=$x.c.controls.projectRoot
}
FstCase 'resolved-assets-escape-project-root' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.controls.resolvedAssetManifests+=@(FstClone $receipt.controls.projectClosure)}} 'control'
FstCase 'cold-topology-reuse-rejected' {param($x)$x.c.lanes[0].authoritative.frames[0].renderer.root.receipt.reusedPersistentPages=1} 'topology'
FstCase 'warm-topology-predicate-required' {param($x)$x.c.lanes[0].authoritative.scenarioAssertions=@($x.c.lanes[0].authoritative.scenarioAssertions|Where-Object id -CNE 'warm-topology-reused')} 'assertion' 'warm-direct-complete'
# Keep forged Passed/assertion flags intact in these cases. Independent actual
# schedule evidence must reject the receipt before authoritative equality runs.
foreach($caseLane in @(0,1)) {
	FstCase ('schedule-construction-missing-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.Remove('scheduleConstruction')} 'field'
	FstCase ('schedule-construction-invalid-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.scheduleConstruction.valid=$false} 'schedule'
	FstCase ('schedule-summary-invalid-valid-plan-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.scheduleValid=$false} 'schedule'
	FstCase ('schedule-plan-invalid-valid-summary-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.valid=$false} 'schedule'
	FstCase ('schedule-summary-validity-missing-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.Remove('scheduleValid')} 'field'
	FstCase ('schedule-construction-no-systems-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.scheduleConstruction.systems=@()} 'schedule'
	FstCase ('schedule-frame-no-systems-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.systemNames=@()} 'schedule'
	FstCase ('schedule-system-construction-join-drift-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.systemNames=@('UnregisteredSystem')} 'difference'
	FstCase ('schedule-empty-declarations-forged-passed-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.declarations=@()} 'empty-proof'
	FstCase ('schedule-empty-results-forged-passed-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.results=@()} 'empty-proof'
	FstCase ('schedule-construction-error-forged-valid-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.scheduleConstruction.diagnostics=@(@{severity=2;category=1;systemName='Synthetic';phaseName='Prepare';resourceName='Input';message='MissingDependency';recommendedAction='RegisterProvider'})} 'schedule'
	FstCase ('schedule-frame-error-forged-valid-lane-'+$caseLane) {param($x)$x.c.lanes[$caseLane].authoritative.frames[0].scheduler.scheduleDiagnostics=@(@{severity=2;category=1;systemName='Synthetic';phaseName='Prepare';resourceName='Input';message='MissingDependency';recommendedAction='RegisterProvider'})} 'schedule'
	foreach($caseTarget in @('PublishAnimationAuthority','RenderPublicationCapture','GeometryRoutePrepare','VirtualGeometryPrepare','BackendCommit','RenderPublication')) {
		FstCase ('required-target-missing-lane-'+$caseLane+'-'+$caseTarget) {param($x)foreach($decl in $x.c.lanes[$caseLane].authoritative.frames[0].scheduler.declarations){if($decl.workloadName -ceq $caseTarget){$decl.workloadName='UnrelatedDescriptor'}}} 'descriptor'
	}
}
FstCase 'schedule-diagnostic-severity-is-typed' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.scheduleDiagnostics=@(@{severity='0';category=1;systemName='Synthetic';phaseName='Prepare';resourceName='Input';message='Info';recommendedAction=''})} 'integer'
FstCase 'schedule-diagnostic-context-required' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.scheduleDiagnostics=@(@{severity=0;category=1;systemName='Synthetic';phaseName='Prepare';resourceName='Input';message='Info'})} 'field'
FstCase 'schedule-diagnostic-unknown-category' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.scheduleDiagnostics=@(@{severity=0;category=99;systemName='Synthetic';phaseName='Prepare';resourceName='Input';message='Info';recommendedAction=''})} 'schedule'
FstCase 'required-target-still-required-during-worker-fault' {param($x)foreach($decl in $x.c.lanes[1].authoritative.frames[0].scheduler.declarations){if($decl.workloadName -ceq 'BackendCommit'){$decl.workloadName='UnrelatedDescriptor'}}} 'descriptor' 'worker-failure'
FstCase 'required-target-duplicate-name' {param($x)$decl=FstClone $x.c.lanes[0].authoritative.frames[0].scheduler.declarations[0];$decl.stableDescriptorId='different-id-same-workload';$x.c.lanes[0].authoritative.frames[0].scheduler.declarations+=@($decl)} 'descriptor'
FstCase 'required-target-case-exact' {param($x)$x.c.lanes[0].authoritative.frames[0].scheduler.declarations[0].workloadName='publishanimationauthority'} 'descriptor'
FstCase 'schedule-warning-retained-and-accepted' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[0].authoritative.scheduleConstruction.diagnostics=@(@{severity=1;category=6;systemName='Synthetic';phaseName='Prepare';resourceName='Input';message='Default metadata observed';recommendedAction='Review'});$receipt.lanes[0].authoritative.frames[0].scheduler.scheduleDiagnostics=@((FstClone $receipt.lanes[0].authoritative.scheduleConstruction.diagnostics[0]))}}
FstCase 'native-fallback-metadata-joined-to-actual-rows' {param($x)foreach($receipt in @($x.r,$x.c)){foreach($group in $receipt.lanes[1].diagnostic.runtimeIdentifiers){foreach($event in $group.events){$event.fallbackSystem='SyntheticNativeProvider'}};foreach($event in $receipt.lanes[1].authoritative.boundaries){$event.fallbackSystem='SyntheticNativeProvider'}}}
FstCase 'native-fallback-canonical-metadata-forgery' {param($x)foreach($receipt in @($x.r,$x.c)){foreach($group in $receipt.lanes[1].diagnostic.runtimeIdentifiers){foreach($event in $group.events){$event.fallbackSystem='SyntheticNativeProvider'}};foreach($event in $receipt.lanes[1].authoritative.boundaries){$event.fallbackSystem='SyntheticNativeProvider'}};$x.c.lanes[1].authoritative.boundaries[0].fallbackSystem='OtherProvider'} 'difference'
function FstOmittedPrerequisite($Context) {
	foreach($receipt in @($Context.r,$Context.c)) {
		$lane=$receipt.lanes[1];$result=$lane.authoritative.frames[0].scheduler.results[0];$result.omitted=$true;$result.executionPolicy=3;$result.completed=$true;$result.partitionCount=0
		$group=$lane.diagnostic.runtimeIdentifiers[0];$group.events=@($group.events[2],$group.events[3]);$group.events[0].observedSequence=1;$group.events[1].observedSequence=2
		$lane.authoritative.boundaries=@($lane.authoritative.boundaries|Select-Object -Skip 2)
		$check=$lane.authoritative.causality[0];$check.observedInvocationCount=1;$check.expectedCompletedInvocationCount=1;$check.checkedDependencyCount=0;$check.omittedDependencyCount=1
	}
}
FstCase 'native-omitted-prerequisite-exact-tuple' {param($x)FstOmittedPrerequisite $x}
FstCase 'native-omission-does-not-count-unentered-dependent' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.dependencyEdges+=,@(2,3)}}
FstCase 'native-omitted-prerequisite-wrong-policy' {param($x)FstOmittedPrerequisite $x;$x.c.lanes[1].authoritative.frames[0].scheduler.results[0].executionPolicy=0} 'causality'
FstCase 'native-omitted-policy-without-flag' {param($x)FstOmittedPrerequisite $x;$x.c.lanes[1].authoritative.frames[0].scheduler.results[0].omitted=$false} 'causality'
FstCase 'native-omitted-prerequisite-not-completed' {param($x)FstOmittedPrerequisite $x;$x.c.lanes[1].authoritative.frames[0].scheduler.results[0].completed=$false} 'causality'
FstCase 'native-omitted-prerequisite-has-partition' {param($x)FstOmittedPrerequisite $x;$x.c.lanes[1].authoritative.frames[0].scheduler.results[0].partitionCount=1} 'causality'
FstCase 'native-omitted-prerequisite-has-actual-events' {param($x)$r=$x.c.lanes[1].authoritative.frames[0].scheduler.results[0];$r.omitted=$true;$r.executionPolicy=3;$r.completed=$true;$r.partitionCount=0} 'causality'
FstCase 'native-omitted-summary-undercount' {param($x)FstOmittedPrerequisite $x;$x.c.lanes[1].authoritative.causality[0].omittedDependencyCount=0} 'causality'
FstCase 'native-omitted-summary-forged-without-omission' {param($x)$x.c.lanes[1].authoritative.causality[0].omittedDependencyCount=1} 'causality'
FstCase 'native-omitted-summary-still-counted-as-temporal' {param($x)FstOmittedPrerequisite $x;$x.c.lanes[1].authoritative.causality[0].checkedDependencyCount=1} 'causality'
FstCase 'native-omitted-summary-field-missing' {param($x)$x.c.lanes[1].authoritative.causality[0].Remove('omittedDependencyCount')} 'causality'
FstCase 'native-contradictory-unused-omission-still-rejected' {param($x)$x.c.lanes[1].authoritative.frames[0].scheduler.results[2].executionPolicy=0} 'causality'
# Forge both sides identically: equality alone must never accept false action facts.
FstCase 'before-command-rejected-predicate-required-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.scenarioAssertions=@($receipt.lanes[1].authoritative.scenarioAssertions|Where-Object id -CNE 'before-exchange-command-rejected')}} 'assertion' 'cancel-before-exchange'
FstCase 'before-command-ok-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.firstCommandReceipt.status=0;$receipt.lanes[1].authoritative.frames[0].commandReceipts[0].status=0}} 'cancellation' 'cancel-before-exchange'
FstCase 'before-command-graph-cancel-status-is-not-producer-rejection' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.firstCommandReceipt.status=9;$receipt.lanes[1].authoritative.frames[0].commandReceipts[0].status=9}} 'cancellation' 'cancel-before-exchange'
FstCase 'before-command-publication-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.firstCommandReceipt.publicationGeneration=1;$receipt.lanes[1].authoritative.frames[0].commandReceipts[0].publicationGeneration=1}} 'cancellation' 'cancel-before-exchange'
FstCase 'before-cancellation-observation-required-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.Remove('cancellationObservation')}} 'field' 'cancel-before-exchange'
FstCase 'before-command-receipt-frame-join-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.firstCommandReceipt.correlationId=999}} 'difference' 'cancel-before-exchange'
FstCase 'cancellation-cache-count-missing-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.cacheCounts.Remove('entriesAfterCallbacks')}} 'field' 'cancel-before-exchange'
FstCase 'cancellation-cache-membership-claim-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.cacheMembershipClaim='ExactEntryMembershipRestored'}} 'cancellation' 'cancel-before-exchange'
FstCase 'after-command-rejected-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.firstCommandReceipt.status=13;$receipt.lanes[1].authoritative.frames[0].commandReceipts[0].status=13}} 'cancellation' 'cancel-after-exchange'
FstCase 'after-command-wrong-authority-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.firstCommandReceipt.publicationGeneration=999;$receipt.lanes[1].authoritative.frames[0].commandReceipts[0].publicationGeneration=999}} 'cancellation' 'cancel-after-exchange'
FstCase 'after-cancellation-observation-required-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.Remove('cancellationObservation')}} 'field' 'cancel-after-exchange'
FstCase 'cancellation-cache-count-type-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.cacheCounts.entriesAfterIngress='1'}} 'integer' 'cancel-before-exchange'
FstCase 'cancellation-cache-counts-do-not-imply-membership-equality' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.cancellationObservation.cacheCounts=@{entriesBeforeIngress=1;entriesAfterIngress=0;entriesAfterCallbacks=3}}} '' 'cancel-before-exchange'
foreach($restartLane in @(0,1)) {
	FstCase ('restart-held-roots-predicate-required-both-sides-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.scenarioAssertions=@($receipt.lanes[$restartLane].authoritative.scenarioAssertions|Where-Object id -CNE 'restart-held-roots-immutable')}} 'assertion' 'restart'
	FstCase ('restart-retained-proof-required-both-sides-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.Remove('restartRetainedRoots')}} 'field' 'restart'
	foreach($retainedField in @('heldRenderImmutable','heldAnimationImmutable','preShutdownRenderImmutable','preShutdownAnimationImmutable')) {
		FstCase ('restart-retained-false-both-sides-lane-'+$restartLane+'-'+$retainedField) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartRetainedRoots[$retainedField]=$false}} 'boolean' 'restart'
	}
	FstCase ('restart-retained-field-missing-both-sides-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartRetainedRoots.Remove('preShutdownAnimationImmutable')}} 'field' 'restart'
	FstCase ('restart-retained-field-type-both-sides-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartRetainedRoots.heldRenderImmutable='true'}} 'boolean' 'restart'
}
FstCase 'expected-invalid-plan-worker-fault-exact-signature' {} '' 'worker-failure'
foreach($faultField in @('code','workloadName','resourceName','message')) {
	FstCase ('expected-worker-fault-wrong-'+$faultField) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.firstFailure[$faultField]=if($faultField -ceq 'code'){7}else{'wrong-actual-failure'}}} 'expected-fault' 'worker-failure'
}
FstCase 'expected-worker-fault-valid-plan-forgery' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.valid=$true}} 'expected-fault' 'worker-failure'
FstCase 'expected-worker-fault-completed-plan-forgery' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.completed=$true}} 'expected-fault' 'worker-failure'
FstCase 'expected-worker-fault-missing-failure' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.Remove('firstFailure')}} 'field' 'worker-failure'
FstCase 'expected-worker-fault-recovery-frame-invalid' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[1].scheduler.valid=$false;$receipt.lanes[1].authoritative.frames[1].scheduler.completed=$false;$receipt.lanes[1].authoritative.frames[1].scheduler.firstFailure=FstClone $receipt.lanes[1].authoritative.frames[0].scheduler.firstFailure}} 'schedule' 'worker-failure'
FstCase 'expected-worker-fault-full-engine-lane-invalid' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[0].authoritative.frames[0].scheduler.valid=$false;$receipt.lanes[0].authoritative.frames[0].scheduler.completed=$false;$receipt.lanes[0].authoritative.frames[0].scheduler.firstFailure=FstClone $receipt.lanes[1].authoritative.frames[0].scheduler.firstFailure}} 'schedule' 'worker-failure'
FstCase 'expected-worker-fault-wrong-scenario-invalid' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.valid=$false;$receipt.lanes[1].authoritative.frames[0].scheduler.completed=$false;$receipt.lanes[1].authoritative.frames[0].scheduler.firstFailure=@{code=8;workloadName='GeometryRoutePrepare';resourceName='RenderSystem/Render/GeometryRoutePrepare#0';message='Job threw during worker execution: frame-replay-real-workload-fault.'}}} 'schedule' 'animated-mixed'
FstCase 'expected-worker-fault-invalid-system-schedule' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.scheduleValid=$false}} 'schedule' 'worker-failure'
FstCase 'expected-worker-fault-result-completed-forgery' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.results[0].completed=$true}} 'expected-fault' 'worker-failure'
FstCase 'expected-worker-fault-result-omitted-forgery' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.results[0].omitted=$true}} 'expected-fault' 'worker-failure'
FstCase 'expected-worker-fault-result-omitted-policy' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.results[0].executionPolicy=3}} 'expected-fault' 'worker-failure'
FstCase 'expected-worker-fault-result-zero-partitions' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.results[0].partitionCount=0}} 'expected-fault' 'worker-failure'
FstCase 'expected-worker-fault-no-failed-exit-proof' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].diagnostic.runtimeIdentifiers[0].events[1].failed=$false;$receipt.lanes[1].authoritative.boundaries[1].failed=$false}} 'causality' 'worker-failure'
FstCase 'expected-worker-fault-actual-failure-on-other-callback' {param($x)foreach($receipt in @($x.r,$x.c)){$lane=$receipt.lanes[1];$lane.diagnostic.runtimeIdentifiers[0].events[1].failed=$false;$lane.authoritative.boundaries[1].failed=$false;$lane.diagnostic.runtimeIdentifiers[0].events[3].failed=$true;$lane.authoritative.boundaries[3].failed=$true}} 'causality' 'worker-failure'
FstCase 'expected-worker-fault-predicate-required' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.scenarioAssertions=@($receipt.lanes[1].authoritative.scenarioAssertions|Where-Object id -CNE 'worker-fault-injected')}} 'assertion' 'worker-failure'
FstCase 'expected-worker-fault-recovery-frame-noncompleted' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[1].scheduler.completed=$false}} 'schedule' 'worker-failure'
FstCase 'ordinary-valid-plan-noncompleted-rejected' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[0].authoritative.frames[0].scheduler.completed=$false}} 'schedule' 'animated-mixed'
FstCase 'fault-profile-proof-missing-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.Remove('profileObservation')}} 'field' 'worker-failure'
FstCase 'fault-profile-wrong-checkpoint-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.checkpoint='AfterMeasuredFrames'}} 'profile' 'worker-failure'
FstCase 'fault-profile-wrong-ordinal-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.frameOrdinal=1}} 'profile' 'worker-failure'
FstCase 'fault-profile-native-frame-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.nativePublicationFrameId=999}} 'profile' 'worker-failure'
FstCase 'fault-profile-native-generation-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.nativePublicationGeneration=999}} 'profile' 'worker-failure'
FstCase 'fault-profile-requested-profile-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.requestedProfile='DirectComplete'}} 'profile' 'worker-failure'
FstCase 'fault-profile-missing-complete-admission-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.completeProcessedAnimators=0;$receipt.lanes[1].authoritative.frames[0].animationAdmission.counters.completeProcessedAnimators=0}} 'profile' 'worker-failure'
FstCase 'fault-profile-missing-compact-admission-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.compactProcessedAnimators=0;$receipt.lanes[1].authoritative.frames[0].animationAdmission.counters.compactProcessedAnimators=0}} 'profile' 'worker-failure'
FstCase 'fault-profile-complete-counter-join-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.completeProcessedAnimators=2}} 'profile' 'worker-failure'
FstCase 'fault-profile-compact-counter-join-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.compactProcessedAnimators=2}} 'profile' 'worker-failure'
FstCase 'fault-profile-counter-field-missing-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].animationAdmission.counters.Remove('compactProcessedAnimators')}} 'field' 'worker-failure'
FstCase 'fault-profile-first-admission-missing-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].Remove('animationAdmission')}} 'field' 'worker-failure'
FstCase 'fault-profile-middle-recovery-admission-missing-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[1].Remove('animationAdmission')}} 'field' 'worker-failure'
FstCase 'fault-profile-count-type-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.completeProcessedAnimators='1'}} 'integer' 'worker-failure'
FstCase 'fault-profile-no-native-route-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.profileObservation.directCommands=0;$receipt.lanes[1].authoritative.profileObservation.virtualCommands=0}} 'profile' 'worker-failure'
FstCase 'fault-profile-held-publication-is-not-fresh-publication-claim' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].renderer.root.frameId=17;$receipt.lanes[1].authoritative.profileObservation.nativePublicationFrameId=17}} '' 'worker-failure'
FstCase 'fault-profile-cpu-recovery-after-clear-is-not-mixed-qualification' {} '' 'worker-failure'
FstCase 'fault-post-retirement-qualifying-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.postRetirementProfileObservation.qualifying=$true}} 'boolean' 'worker-failure'
FstCase 'fault-post-retirement-checkpoint-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.postRetirementProfileObservation.checkpoint='FaultFrameBeforeRendererClear'}} 'profile' 'worker-failure'
FstCase 'fault-post-retirement-ordinal-forgery-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.postRetirementProfileObservation.frameOrdinal=0}} 'profile' 'worker-failure'
FstCase 'fault-post-retirement-raw-admission-join-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.postRetirementProfileObservation.animationAdmission.counters.compactProcessedAnimators=1}} 'difference' 'worker-failure'
FstCase 'fault-post-retirement-observation-missing-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.Remove('postRetirementProfileObservation')}} 'field' 'worker-failure'

# Two-sided forgeries must fail semantic admission even when equality holds.
FstCase 'restart-equal-backend-local-session-two' {param($x)foreach($receipt in @($x.r,$x.c)){foreach($lane in $receipt.lanes){$lane.authoritative.frames[2].renderer.root.rendererSessionGeneration=2;$lane.authoritative.restartPublications.renderer.root.rendererSessionGeneration=2;$lane.authoritative.restartPublicationIdentity.old.rendererSessionGeneration=2;$lane.authoritative.restartPublicationIdentity.fresh.rendererSessionGeneration=2}}} '' 'restart'
foreach($restartLane in @(0,1)) {
	foreach($requiredId in @('restart-new-render-publication','restart-shutdown-roots-retired')) {
		FstCase ('restart-required-'+$requiredId+'-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.scenarioAssertions=@($receipt.lanes[$restartLane].authoritative.scenarioAssertions|Where-Object {$_.id -cne $requiredId})}} 'assertion' 'restart'
	}
	foreach($proof in @('restartShutdownObservation','restartPublicationIdentity','restartPublications')) {
		FstCase ('restart-missing-'+$proof+'-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.Remove($proof)}} 'field' 'restart'
	}
	foreach($fact in @('rendererRootRetired','animationRootRetired','contextReleased')) {
		FstCase ('restart-retirement-false-'+$fact+'-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartShutdownObservation[$fact]=$false}} 'boolean' 'restart'
	}
	foreach($fact in @('freshContextPresent','distinctRetainedPublication')) {
		FstCase ('restart-new-publication-false-'+$fact+'-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartPublicationIdentity[$fact]=$false}} 'boolean' 'restart'
	}
	FstCase ('restart-wrong-domain-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartPublicationIdentity.rendererGenerationDomain='GlobalMonotonicSession'}} 'restart' 'restart'
	foreach($side in @('old','fresh')) {foreach($fact in @('rendererSessionGeneration','backendResourceGeneration','generation','frameId')) {
		FstCase ('restart-native-identity-forgery-'+$side+'-'+$fact+'-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartPublicationIdentity[$side][$fact]=999}} 'difference' 'restart'
	}}
	foreach($restartKind in @('renderer','animation')) {
		FstCase ('restart-absent-fresh-root-'+$restartKind+'-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$restartLane].authoritative.restartPublications[$restartKind]=@{present=$false;root=$null}}} 'restart' 'restart'
	}
	FstCase ('restart-reused-animation-session-lane-'+$restartLane) {param($x)foreach($receipt in @($x.r,$x.c)){$a=$receipt.lanes[$restartLane].authoritative;$a.restartPublications.animation.root.sessionGeneration=$a.frames[2].animation.root.sessionGeneration}} 'restart' 'restart'
}

FstCase 'native-worker-descriptor-distinct-from-partition-job' {} '' 'worker-failure'
FstCase 'native-worker-descriptor-conflated-with-partition-job-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$a=$receipt.lanes[1].authoritative;$a.frames[0].scheduler.declarations[0].stableDescriptorId='RenderSystem/Render/GeometryRoutePrepare#0';$a.frames[0].scheduler.results[0].stableDescriptorId='RenderSystem/Render/GeometryRoutePrepare#0';foreach($e in @($receipt.lanes[1].diagnostic.runtimeIdentifiers[0].events[0],$receipt.lanes[1].diagnostic.runtimeIdentifiers[0].events[1],$a.boundaries[0],$a.boundaries[1])){$e.stableDescriptorId='RenderSystem/Render/GeometryRoutePrepare#0'}}} 'expected-fault' 'worker-failure'
FstCase 'native-worker-wrong-partition-resource-both-sides' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.firstFailure.resourceName='RenderSystem/Render/GeometryRoutePrepare#1'}} 'expected-fault' 'worker-failure'
foreach($wrongPolicy in @(0,2,4)) {
	FstCase ('native-worker-contradictory-policy-'+$wrongPolicy) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.results[0].executionPolicy=$wrongPolicy}} 'expected-fault' 'worker-failure'
}
foreach($wrongFlag in @('workerEligible','mainThreadRequired')) {
	FstCase ('native-worker-contradictory-declaration-'+$wrongFlag) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.declarations[0][$wrongFlag]=($wrongFlag -ceq 'mainThreadRequired')}} 'expected-fault' 'worker-failure'
}
foreach($wrongFlag in @('inlineSelected','fallbackUsed')) {
	FstCase ('native-worker-contradictory-result-'+$wrongFlag) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.results[0][$wrongFlag]=$true}} 'expected-fault' 'worker-failure'
}
FstCase 'native-worker-inline-threshold-not-exceeded' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.declarations[0].estimatedWorkItemCount=16}} 'expected-fault' 'worker-failure'
FstCase 'native-worker-zero-estimated-work' {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[1].authoritative.frames[0].scheduler.declarations[0].estimatedWorkItemCount=0}} 'integer' 'worker-failure'

FstCase 'native-worker-root-sixteen-embedded-receipt-zero' {} '' 'worker-failure'
FstCase 'native-worker-root-generation-conflated-with-receipt' {param($x)foreach($receipt in @($x.r,$x.c)){$p=$receipt.lanes[1].authoritative.profileObservation;$p.nativePublicationGeneration=$p.nativePublicationReceiptGeneration}} 'profile' 'worker-failure'
FstCase 'native-worker-receipt-generation-conflated-with-root' {param($x)foreach($receipt in @($x.r,$x.c)){$p=$receipt.lanes[1].authoritative.profileObservation;$p.nativePublicationReceiptGeneration=$p.nativePublicationGeneration}} 'profile' 'worker-failure'

# Native probe15 and Declaration() use inlineWorkItemThreshold at declaration
# level. Keep a complete serialized metadata control so invented keys cannot pass.
$nativeSerializedFaultMetadata=@'
{"declaration":{"activationPredicateKey":"render-system-frame","active":true,"boundaryClassification":2,"estimatedWorkItemCount":19,"executionOrder":11266,"explicitDependencies":["RenderSystem/Render/RenderPublicationCapture"],"fallbackReason":"","hardBoundary":false,"inlineWorkItemThreshold":16,"mainThreadRequired":false,"maximumPartitionCount":0,"mutationPolicy":0,"ownerSystem":"RenderSystem","partitionPolicy":0,"partitionWorkItemCount":0,"phase":"Render","phaseKind":1,"profilingLabel":"RenderSystem.GeometryRoutePrepare","protectedCommit":"","protectedReason":"","readinessReason":"","reads":["RenderCaptureInputs","RendererEfficiencyOptions","RendererFrameScratch","VirtualGeometryQualityProfile"],"ready":true,"safeSubwork":"Assembles immutable render records, scores representation candidates, applies calibrated hysteresis, commits transactional topology pages, and groups direct instances without touching live resources. Borrows completed camera/light scratch collections read-only; no allocation/reset occurs on workers.","sameFrameVisibilityRequired":false,"stableDescriptorGeneration":1,"stableDescriptorId":"RenderSystem/Render/GeometryRoutePrepare","workerEligible":true,"workloadName":"GeometryRoutePrepare","writes":["DirectMeshletBatches","DynamicRenderFrameState","GeometryRouteDecisions","PersistentRenderSceneState","PersistentRenderTopology","RenderCompatibilityGroups"]},"result":{"completed":false,"executionPolicy":1,"fallbackUsed":false,"inlineSelected":false,"omitted":false,"partitionCount":1,"stableDescriptorId":"RenderSystem/Render/GeometryRoutePrepare"}}
'@ | ConvertFrom-Json -AsHashtable
FstCase 'native-serialized-worker-declaration-and-result-schema' {param($x)foreach($receipt in @($x.r,$x.c)){$lane=$receipt.lanes[1];$lane.authoritative.frames[0].scheduler.declarations[0]=FstClone $nativeSerializedFaultMetadata.declaration;$lane.authoritative.frames[0].scheduler.results[0]=FstClone $nativeSerializedFaultMetadata.result;foreach($event in @($lane.diagnostic.runtimeIdentifiers[0].events[0],$lane.diagnostic.runtimeIdentifiers[0].events[1],$lane.authoritative.boundaries[0],$lane.authoritative.boundaries[1])){$event.executionOrder=$nativeSerializedFaultMetadata.declaration.executionOrder;$event.itemEnd=$nativeSerializedFaultMetadata.declaration.estimatedWorkItemCount};$lane.authoritative.boundaries=@($lane.authoritative.boundaries|Sort-Object frameOrdinal,executionOrder,stableDescriptorId,partitionIndex,checkpoint)}} '' 'worker-failure'
FstCase 'native-worker-inferred-threshold-key-rejected' {param($x)foreach($receipt in @($x.r,$x.c)){$declaration=$receipt.lanes[1].authoritative.frames[0].scheduler.declarations[0];$declaration.inlineThreshold=$declaration.inlineWorkItemThreshold;$declaration.Remove('inlineWorkItemThreshold')}} 'field' 'worker-failure'

FstCase 'native-multi-output-repeated-destination-two-identical-camera-values' {} '' 'multi-output'
foreach($multiLane in @(0,1)) {
	foreach($multiMissing in @('multiOutputObservation','capturedInputs')) {
		FstCase ('multi-output-missing-'+$multiMissing+'-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].Remove($multiMissing)}} 'field' 'multi-output'
	}
	FstCase ('multi-output-missing-control-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.Remove('sceneViewControl')}} 'field' 'multi-output'
	foreach($multiFlag in @('cameraEnabled','activeTransformCamera')) {
		FstCase ('multi-output-inactive-'+$multiFlag+'-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.cameraObservations[0][$multiFlag]=$false}} 'boolean' 'multi-output'
	}
	FstCase ('multi-output-forged-passed-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.passed=$false}} 'boolean' 'multi-output'
	FstCase ('multi-output-wrong-association-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.association='InventedCameraUUIDPerView'}} 'multi-output' 'multi-output'
	FstCase ('multi-output-authored-id-forgery-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.expectedAuthoredCameraIds[0]='forged'}} 'multi-output' 'multi-output'
	FstCase ('multi-output-duplicate-captured-camera-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$f=$receipt.lanes[$multiLane].authoritative.frames[0];$f.capturedInputs.cameras[1]=FstClone $f.capturedInputs.cameras[0];$f.multiOutputObservation.cameraObservations[1].capturedCamera=FstClone $f.capturedInputs.cameras[0];$f.multiOutputObservation.cameraObservations[1].protectedCamera=FstClone $f.capturedInputs.cameras[0]}} 'multi-output' 'multi-output'
	foreach($multiField in @('capturedCamera','protectedCamera')) {
		FstCase ('multi-output-camera-join-'+$multiField+'-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.cameraObservations[0][$multiField].verticalFovBits=999}} 'difference' 'multi-output'
	}
	foreach($multiField in @('cameraOrdinal','presentationIndex')) {
		FstCase ('multi-output-ordinal-forgery-'+$multiField+'-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.cameraObservations[0][$multiField]=9}} 'multi-output' 'multi-output'
	}
	FstCase ('multi-output-pose-missing-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.cameraObservations[0].Remove('canonicalPose')}} 'field' 'multi-output'
	FstCase ('multi-output-pose-incomplete-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.cameraObservations[0].canonicalPose.rotationBits=@(0)}} 'multi-output' 'multi-output'
	foreach($multiField in @('viewMatrix','projectionMatrix')) {
		FstCase ('multi-output-runtime-matrix-join-'+$multiField+'-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].renderer.root.presentationInputs[1][$multiField][0][0].ieee754=999}} 'difference' 'multi-output'
		FstCase ('multi-output-scene-matrix-join-'+$multiField+'-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.sceneViewControl[$multiField+'Bits'][0]=999}} 'difference' 'multi-output'
	}
	foreach($multiField in @('outputId','kind','diagnosticsView')) {
		FstCase ('multi-output-presentation-metadata-'+$multiField+'-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].renderer.root.presentationInputs[2].output[$multiField]=999}} 'multi-output' 'multi-output'
	}
	FstCase ('multi-output-view-count-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].renderer.root.views=@($receipt.lanes[$multiLane].authoritative.frames[0].renderer.root.views[0..1])}} 'multi-output' 'multi-output'
	FstCase ('multi-output-range-outside-membership-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$f=$receipt.lanes[$multiLane].authoritative.frames[0];$f.renderer.root.submissionRanges[0].viewIndex=3;$f.multiOutputObservation.rangeViewIndices[0]=3}} 'multi-output' 'multi-output'
	FstCase ('multi-output-range-join-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.rangeViewIndices[0]=2}} 'difference' 'multi-output'
	FstCase ('multi-output-second-frame-evidence-required-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[1].Remove('multiOutputObservation')}} 'field' 'multi-output'
	FstCase ('multi-output-order-change-preserved-lane-'+$multiLane) {param($x)$f=$x.c.lanes[$multiLane].authoritative.frames[0];[Array]::Reverse($f.capturedInputs.cameras);$f.multiOutputObservation.cameraObservations[0].capturedCamera=FstClone $f.capturedInputs.cameras[0];$f.multiOutputObservation.cameraObservations[0].protectedCamera=FstClone $f.capturedInputs.cameras[0];$f.multiOutputObservation.cameraObservations[1].capturedCamera=FstClone $f.capturedInputs.cameras[1];$f.multiOutputObservation.cameraObservations[1].protectedCamera=FstClone $f.capturedInputs.cameras[1]} 'difference' 'multi-output'
}
FstCase 'duplicate-runtime-destination-without-closed-multioutput-rejected' {param($x)foreach($receipt in @($x.r,$x.c)){FstMultiOutput $receipt.lanes[0]}} 'duplicate-id'

foreach($multiLane in @(0,1)) {
	FstCase ('multi-output-scene-pose-required-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.sceneViewControl.Remove('canonicalPose')}} 'field' 'multi-output'
	FstCase ('multi-output-camera-typed-bits-required-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].capturedInputs.cameras[0].verticalFovBits=$null}} 'integer' 'multi-output'
	FstCase ('multi-output-pose-typed-bits-required-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.cameraObservations[0].canonicalPose.rotationBits[0]=$null}} 'integer' 'multi-output'
	FstCase ('multi-output-pose-typed-cell-required-lane-'+$multiLane) {param($x)foreach($receipt in @($x.r,$x.c)){$receipt.lanes[$multiLane].authoritative.frames[0].multiOutputObservation.cameraObservations[0].canonicalPose.cell[0]='0'}} 'multi-output' 'multi-output'
}
function FstRequireMinimumTests([int]$Actual,[int]$Minimum){if($Actual -lt $Minimum){throw '[FrameReplayTests:floor] Protocol case count fell below the required minimum.'}}
$floorRejected=$false;try{FstRequireMinimumTests 262 263}catch{if($_.Exception.Message -like '[[]FrameReplayTests:floor[]]*'){$floorRejected=$true}else{throw}}
if(!$floorRejected){throw 'MinimumTests guard accepted fewer than 263 cases.'}
$testResults.Add([pscustomobject]@{name='minimum-tests-floor-rejects-underfloor';passed=$true;expectedRejection='FrameReplayTests:floor'})
FstRequireMinimumTests $testResults.Count $MinimumTests
$summary=[pscustomobject]@{schemaVersion='frame-stage-replay-comparison-selftest-v1';status='Passed';tests=$testResults.Count;protocolOnly=$true;replayAcceptance=$false;gpuAcceptance=$false;realProcessesLaunched=$false;resultRoot=$testRoot;cases=$testResults}
FstWrite (Join-Path $testRoot 'result.json') $summary
$summary|Select-Object schemaVersion,status,tests,protocolOnly,replayAcceptance,gpuAcceptance,realProcessesLaunched,resultRoot|ConvertTo-Json
