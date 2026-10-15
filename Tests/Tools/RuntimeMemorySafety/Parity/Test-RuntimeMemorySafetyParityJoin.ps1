<# Bounded parity protocol checks. Synthetic files never count as GPU/parity acceptance. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkRoot)
$testRoot=[IO.Path]::GetFullPath($WorkRoot)
. (Join-Path $PSScriptRoot 'Join-RuntimeMemorySafetyParity.ps1')
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
if(Test-Path $testRoot){throw 'Test work root must be fresh'};$null=[IO.Directory]::CreateDirectory($testRoot)
$script:parityPins=@{};$script:checks=0
function Check([string]$Name,[scriptblock]$Body){& $Body;$script:checks++;Write-Output "[PASS] $Name"}
function Reject([string]$Name,[scriptblock]$Body){$caught=$false;try{& $Body|Out-Null}catch{$caught=$true};if(!$caught){throw "Missing rejection $Name"};$script:checks++;Write-Output "[PASS] $Name rejected"}
function WriteModel([string]$Name,$Value){$path=Join-Path $testRoot $Name;[IO.File]::WriteAllText($path,($Value|ConvertTo-Json -Depth 50),[Text.UTF8Encoding]::new($false));return RmsParityPin $path}
function Clone($Value){return $Value|ConvertTo-Json -Depth 50|ConvertFrom-Json}
$authored=[pscustomobject]@{observationKind='protectedAuthoredInputProjection';cameras=@([pscustomobject]@{ordinal=0;objectId='camera';pose=@(1,2);componentBits=@(7)});lights=@([pscustomobject]@{ordinal=0;objectId='light';pose=@(3,4);componentBits=@(8)})}
$expected=[pscustomobject]@{observationKind='independentProtectedQuery';cameras=@([pscustomobject]@{ordinal=0;objectId='camera';componentBits=@(7)});lights=@([pscustomobject]@{ordinal=0;objectId='light';componentBits=@(8)})}
$captured=Clone $expected;$captured.observationKind='actualNativeCapturedArrays'
$authoredPin=WriteModel authored.json $authored;$expectedPin=WriteModel expected.json $expected;$capturedPin=WriteModel captured.json $captured
$capture=[pscustomobject]@{lifetimeId=1;episodeId=2;frameId=3;capacityBytes=16777216;usedBytes=100;peakBytes=100;lifetimeHighWaterBytes=100;reuseAllocationCount=0;fallbackAllocationCount=2;resetCount=0;outcomeCode=1}
$terminal=Clone $capture;$terminal.outcomeCode=2
$reclaim=[pscustomobject]@{lifetimeId=1;totalFallbackAllocationCount=2;totalFallbackDeallocationCount=0;outstandingFallbackAllocationCount=2;outstandingFallbackBytes=64;resetCount=0}
$retired=Clone $reclaim;$retired.totalFallbackDeallocationCount=2;$retired.outstandingFallbackAllocationCount=0;$retired.outstandingFallbackBytes=0
$native=[pscustomobject]@{typedIdentityOrderComponentBitsEqual=$true;expectedInputs=$expectedPin;capturedInputs=$capturedPin;capturedEpisode=$capture;terminalEpisode=$terminal;selectedFrameReclamation=$reclaim;retirementReclamation=$retired}
$scratch=[pscustomobject]@{capturedArraysEqual=$true;actualFallbackAllocationCount=2;expectedInputs=$expectedPin;capturedInputs=$capturedPin;capturedEpisode=$capture;terminalEpisode=$terminal;selectedFrameReclamation=$reclaim;retirementReclamation=$retired}
$comparison=[pscustomobject]@{candidateCapturedArraysObservedAndEqual=$true;authoredCameraLightValuesIdentityAndOrderEqual=$true;actualOrderedGpuLightPayloadAndIdsEqual=$true;actualCandidateFallbackAllocationCount=2;candidateScratch=$scratch}
$candidate=[pscustomobject]@{authoredInputs=$authoredPin;candidateScratch=$native;selectedFrameSerial=3};$definition=[pscustomobject]@{cameraCount=1;lightCount=1}
Check 'synthetic-typed-array-and-actual-counter-protocol' {RmsParityRequire ((RmsParityScratch $comparison $candidate $definition) -eq 2) 'Lost terminal fallback count'}
foreach($fault in @('claimed-arrays','claimed-order','claimed-payload','typed-identity','fallback-bool','fallback-mismatch','terminal-identity','terminal-failed','retired-live','scale-count')){
	$c=Clone $comparison;$h=Clone $candidate;$d=Clone $definition
	switch($fault){
		'claimed-arrays'{$c.candidateCapturedArraysObservedAndEqual=$false}
		'claimed-order'{$c.authoredCameraLightValuesIdentityAndOrderEqual=$false}
		'claimed-payload'{$c.actualOrderedGpuLightPayloadAndIdsEqual=$false}
		'typed-identity'{$h.candidateScratch.typedIdentityOrderComponentBitsEqual=$false}
		'fallback-bool'{$c.actualCandidateFallbackAllocationCount=$true}
		'fallback-mismatch'{$c.actualCandidateFallbackAllocationCount=3}
		'terminal-identity'{$c.candidateScratch.terminalEpisode.frameId=4;$h.candidateScratch.terminalEpisode.frameId=4}
		'terminal-failed'{$c.candidateScratch.terminalEpisode.outcomeCode=3;$h.candidateScratch.terminalEpisode.outcomeCode=3}
		'retired-live'{$c.candidateScratch.retirementReclamation.outstandingFallbackAllocationCount=1;$h.candidateScratch.retirementReclamation.outstandingFallbackAllocationCount=1}
		'scale-count'{$d.lightCount=2}
	}
	Reject "scratch-$fault" {RmsParityScratch $c $h $d}
}
$raw=Join-Path $testRoot 'rgba.bin';[IO.File]::WriteAllBytes($raw,[byte[]]@(1,2,3,4));$rawPin=RmsParityPin $raw
$image=[pscustomobject]@{nativeExtent=@(1,1);image=[pscustomobject]@{comparedPixels=1;comparedChannels=4;maximumChannelError=2;maximumNormalizedChannelError=(2.0/255);channelsExceedingTolerance=0;firstFailure=$null};baselineCommand=[pscustomobject]@{frameSerial=1;commandSerial=2;submitSerial=3;completionSerial=4};candidateCommand=[pscustomobject]@{frameSerial=5;commandSerial=6;submitSerial=7;completionSerial=8};baselineRawColor=$rawPin;candidateRawColor=$rawPin;baselineRgba8=$rawPin;candidateRgba8=$rawPin}
Check 'synthetic-image-inclusive2of255-complete-channel-protocol' {RmsParityImage $image}
foreach($fault in @('widened-delta','omitted-alpha','pixel-count','normalized-delta','first-failure','zero-completion','rgba-count')){
	$copy=Clone $image
	switch($fault){'widened-delta'{$copy.image.maximumChannelError=3}'omitted-alpha'{$copy.image.comparedChannels=3}'pixel-count'{$copy.image.comparedPixels=2}'normalized-delta'{$copy.image.maximumNormalizedChannelError=0}'first-failure'{$copy.image.firstFailure=@{pixel=0}}'zero-completion'{$copy.candidateCommand.completionSerial=0}'rgba-count'{$copy.candidateRgba8.bytes=3}}
	Reject "image-$fault" {RmsParityImage $copy}
}
foreach($kind in @('duplicate','nonfinite')){$path=Join-Path $testRoot "$kind.json";[IO.File]::WriteAllText($path,$(if($kind -ceq 'duplicate'){'{"proof":true,"proof":false}'}else{'{"count":NaN}'}));Reject "json-$kind" {RmsParityRead $path}}
Reject 'boolean-is-not-counter' {RmsParityUint ([pscustomobject]@{count=$true}) count}
Reject 'string-is-not-observation' {RmsParityBool ([pscustomobject]@{observed='true'}) observed}
$changed=Join-Path $testRoot 'changed.bin';[IO.File]::WriteAllText($changed,'before');$changedPin=RmsParityPin $changed;[IO.File]::WriteAllText($changed,'after');Reject 'changed-runtime-or-raw-pin' {RmsParityKeep $changedPin}
$coverage=[pscustomobject]@{authoredCameraCount=4;normalQueuedCameraCommands=4;backendSnapshotCapacity=8;expectedAdmittedCameraCommands=4;actualRecordedCameraCommands=4;actualNormalPublishedViewCount=1;actualCommandCount=4;lightingArtifactCommandCount=1;lightingPurposeArtifactCount=2;perCameraImageCoverage=$false;privateReferenceArraysObserved=$false;queuePolicy='AllAuthoredCamerasInCapturedOrderSharedGameOutput2';readbackLimit='Lighting payload readback deduplicates frame/view/purpose; final Game View covers the completed normal shared target';capacityLimit='Editor required-output overflow submits and completes a prefix, then reuses the same eight physical snapshot slices';capacityPolicy='EditorRequiredOutputOverflowSerializesAndReusesEightPhysicalSlices'}
$metadata=@();foreach($ordinal in 0..3){foreach($purpose in @($false,$true)){$metadata+=,[pscustomobject]@{commandOrdinal=$ordinal;frameSerial=99;commandSerial=(100+$ordinal);analysisPurpose=$purpose;status=$(if($ordinal -eq 0){1}else{2});reason=$(if($ordinal -eq 0){''}else{'Duplicate native lighting purpose observation.'});submitSerial=$(if($ordinal -eq 0){1}else{0});completionSerial=$(if($ordinal -eq 0){2}else{0});preparationSettled=$true;viewType=0;viewId=2;fixtureControlGeneration=1;settingsGeneration=2;optionsSessionGeneration=3;hasArtifacts=($ordinal -eq 0)}}}
$normal=[pscustomobject]@{policy='EditorNormalGameViewAfterAllAuthoredCameraSubmissions';frameId=99;ok=$true;backendReadback=$true;deterministicFallback=$false;pendingAsync=$false;freshness='latestCompleted';dedicatedRenderTarget=$false;allowBackendFallback=$false;overlaysIncluded=$false;size=@(1280,720);mimeType='image/png';png=$rawPin;presentation=[pscustomobject]@{submitted=$true;nativeResult=0;submissionSerial=10};snapshotDiagnostics=[pscustomobject]@{valid=$true;maxSnapshotsPerFrame=8;peakSnapshotCount=4;activeSnapshotCount=4;completedSnapshotCount=4;fallbackEvents=@()}}
$normalHosts=@{Baseline=[pscustomobject]@{selectedFrameSerial=99;fixtureControlAdmission=[pscustomobject]@{controlGeneration=1;settingsGeneration=2};normalCommandMetadata=$metadata;normalOutputCoverage=$coverage;finalNormalOutput=$normal};Candidate=[pscustomobject]@{selectedFrameSerial=99;fixtureControlAdmission=[pscustomobject]@{controlGeneration=1;settingsGeneration=2};normalCommandMetadata=(Clone $metadata);normalOutputCoverage=(Clone $coverage);finalNormalOutput=(Clone $normal)}}
$normalComparison=[pscustomobject]@{normalCommandCohortEqual=$true;normalOutputCoverage=$coverage;baselineFinalNormalOutput=$normal;candidateFinalNormalOutput=(Clone $normal);finalNormalOutputImage=[pscustomobject]@{comparedPixels=921600;comparedChannels=3686400;maximumChannelError=2;maximumNormalizedChannelError=(2.0/255);channelsExceedingTolerance=0;firstFailure=$null}}
Check 'synthetic-all-four-commands-two-first-artifacts-one-final-view' {RmsParityNormalCoverage $normalComparison $normalHosts ([pscustomobject]@{cameraCount=4});RmsParityFinalOutput $normalComparison $normalHosts}
foreach($fault in @('missing-actual-command','metadata-order','metadata-wrong-frame','metadata-no-generation','metadata-first-unavailable','metadata-later-fake-artifact','metadata-later-fake-completion','metadata-later-wrong-reason','coverage-per-camera-image-claim','coverage-wrong-commandcount','missing-final-output','nonbackend-final-output','backend-fallback-allowed','deterministic-fallback','dedicated-final-output','pending-final-output','wrong-final-frame','no-normal-presentation','final-alpha-omitted','final-over-tolerance')){
	$c=Clone $normalComparison;$h=@{Baseline=(Clone $normalHosts.Baseline);Candidate=(Clone $normalHosts.Candidate)}
	switch($fault){
		'missing-actual-command'{$h.Candidate.normalCommandMetadata=@($h.Candidate.normalCommandMetadata|Select-Object -First 6)}
		'metadata-order'{$h.Candidate.normalCommandMetadata[2].commandSerial=1}
		'metadata-wrong-frame'{$h.Candidate.normalCommandMetadata[2].frameSerial=98}
		'metadata-no-generation'{$h.Candidate.normalCommandMetadata[2].settingsGeneration=0}
		'metadata-first-unavailable'{$h.Candidate.normalCommandMetadata[0].status=2}
		'metadata-later-fake-artifact'{$h.Candidate.normalCommandMetadata[2].hasArtifacts=$true}
		'metadata-later-fake-completion'{$h.Candidate.normalCommandMetadata[2].completionSerial=1}
		'metadata-later-wrong-reason'{$h.Candidate.normalCommandMetadata[2].reason='Placeholder'}
		'coverage-per-camera-image-claim'{$c.normalOutputCoverage.perCameraImageCoverage=$true;$h.Baseline.normalOutputCoverage.perCameraImageCoverage=$true;$h.Candidate.normalOutputCoverage.perCameraImageCoverage=$true}
		'coverage-wrong-commandcount'{$c.normalOutputCoverage.actualCommandCount=1;$h.Baseline.normalOutputCoverage.actualCommandCount=1;$h.Candidate.normalOutputCoverage.actualCommandCount=1}
		'missing-final-output'{$c.PSObject.Properties.Remove('candidateFinalNormalOutput')}
		'nonbackend-final-output'{$c.candidateFinalNormalOutput.backendReadback=$false;$h.Candidate.finalNormalOutput.backendReadback=$false}
		'backend-fallback-allowed'{$c.candidateFinalNormalOutput.allowBackendFallback=$true;$h.Candidate.finalNormalOutput.allowBackendFallback=$true}
		'deterministic-fallback'{$c.candidateFinalNormalOutput.deterministicFallback=$true;$h.Candidate.finalNormalOutput.deterministicFallback=$true}
		'dedicated-final-output'{$c.candidateFinalNormalOutput.dedicatedRenderTarget=$true;$h.Candidate.finalNormalOutput.dedicatedRenderTarget=$true}
		'pending-final-output'{$c.candidateFinalNormalOutput.pendingAsync=$true;$h.Candidate.finalNormalOutput.pendingAsync=$true}
		'wrong-final-frame'{$c.candidateFinalNormalOutput.frameId=98;$h.Candidate.finalNormalOutput.frameId=98}
		'no-normal-presentation'{$c.candidateFinalNormalOutput.presentation.submitted=$false;$h.Candidate.finalNormalOutput.presentation.submitted=$false}
		'final-alpha-omitted'{$c.finalNormalOutputImage.comparedChannels=3}
		'final-over-tolerance'{$c.finalNormalOutputImage.maximumChannelError=3}
	}
	Reject "normal-output-$fault" {RmsParityNormalCoverage $c $h ([pscustomobject]@{cameraCount=4});RmsParityFinalOutput $c $h}
}
$stressCoverage=Clone $coverage;foreach($name in @('authoredCameraCount','normalQueuedCameraCommands')){$stressCoverage.$name=16};foreach($name in @('expectedAdmittedCameraCommands','actualRecordedCameraCommands','actualCommandCount')){$stressCoverage.$name=16}
$stressMetadata=@();foreach($ordinal in 0..15){foreach($purpose in 0..1){$row=Clone $metadata[$(if($ordinal -eq 0){$purpose}else{2+$purpose})];$row.commandOrdinal=$ordinal;$row.commandSerial=100+$ordinal;$stressMetadata+=,$row}}
$stressComparison=Clone $normalComparison;$stressComparison.normalOutputCoverage=$stressCoverage
$stressHosts=@{Baseline=(Clone $normalHosts.Baseline);Candidate=(Clone $normalHosts.Candidate)};foreach($variant in @('Baseline','Candidate')){$stressHosts[$variant].normalOutputCoverage=(Clone $stressCoverage);$stressHosts[$variant].normalCommandMetadata=(Clone $stressMetadata)}
$stressDiagnostics=[pscustomobject]@{valid=$true;maxSnapshotsPerFrame=8;peakSnapshotCount=16;activeSnapshotCount=16;completedSnapshotCount=16;fallbackEvents=@([pscustomobject]@{kind=2;reason=1;result=1;outputId=2;required=$true})}
foreach($variant in @('Baseline','Candidate')){$stressHosts[$variant].finalNormalOutput.snapshotDiagnostics=(Clone $stressDiagnostics);$stressComparison.($variant.ToLowerInvariant()+'FinalNormalOutput').snapshotDiagnostics=(Clone $stressDiagnostics)}
$runtimeOnlyComparison=Clone $stressComparison;$runtimeOnlyHosts=@{Baseline=(Clone $stressHosts.Baseline);Candidate=(Clone $stressHosts.Candidate)}
foreach($name in @('expectedAdmittedCameraCommands','actualRecordedCameraCommands','actualCommandCount')){$runtimeOnlyComparison.normalOutputCoverage.$name=8;foreach($variant in @('Baseline','Candidate')){$runtimeOnlyHosts[$variant].normalOutputCoverage.$name=8}}
Reject 'runtime-only-eight-command-cap-cannot-qualify-editor-stress' {RmsParityNormalCoverage $runtimeOnlyComparison $runtimeOnlyHosts ([pscustomobject]@{cameraCount=16})}
Check 'synthetic-editor-stress-all16-recorded-through-eight-physical-slices' {RmsParityNormalCoverage $stressComparison $stressHosts ([pscustomobject]@{cameraCount=16});RmsParityFinalOutput $stressComparison $stressHosts}
$dynamicComparison=Clone $stressComparison;$dynamicComparison.normalOutputCoverage.lightingArtifactCommandCount=2;$dynamicComparison.normalOutputCoverage.lightingPurposeArtifactCount=4
$dynamicHosts=@{Baseline=(Clone $stressHosts.Baseline);Candidate=(Clone $stressHosts.Candidate)}
foreach($variant in @('Baseline','Candidate')){$dynamicHosts[$variant].normalOutputCoverage=(Clone $dynamicComparison.normalOutputCoverage);foreach($index in @(16,17)){$row=$dynamicHosts[$variant].normalCommandMetadata[$index];$row.status=1;$row.reason='';$row.hasArtifacts=$true;$row.submitSerial=9;$row.completionSerial=10}}
$artifactRows=@();foreach($ordinal in @(0,8)){foreach($purpose in @($false,$true)){$row=Clone $image;$row|Add-Member cohortOrdinal $ordinal;$row|Add-Member observedRowOrdinal $artifactRows.Count;$row|Add-Member analysisPurpose $purpose;$row|Add-Member viewType 0;$row|Add-Member viewId 2;foreach($variant in @('Baseline','Candidate')){$meta=@($dynamicHosts[$variant].normalCommandMetadata|Where-Object {$_.commandOrdinal -eq $ordinal -and $_.analysisPurpose -eq $purpose})[0];$command=$row.($variant.ToLowerInvariant()+'Command');foreach($field in @('frameSerial','commandSerial','submitSerial','completionSerial')){$command.$field=$meta.$field}};$artifactRows+=,$row}}
$dynamicComparison|Add-Member comparisons $artifactRows
Check 'dynamic-real-artifact-ordinals-zero-and-eight-both-compared' {RmsParityNormalCoverage $dynamicComparison $dynamicHosts ([pscustomobject]@{cameraCount=16});RmsParityLightingCohort $dynamicComparison $dynamicHosts ([pscustomobject]@{cameraCount=16})}
foreach($fault in @('missing-available-purpose','omitted-second-available-command','wrong-artifact-command','unavailable-command-claimed-as-artifact','missing-editor-serialization')){
	$c=Clone $dynamicComparison;$h=@{Baseline=(Clone $dynamicHosts.Baseline);Candidate=(Clone $dynamicHosts.Candidate)}
	switch($fault){
		'missing-available-purpose'{$c.comparisons=@($c.comparisons|Select-Object -First 3)}
		'omitted-second-available-command'{$c.normalOutputCoverage.lightingArtifactCommandCount=1;$c.normalOutputCoverage.lightingPurposeArtifactCount=2;$c.comparisons=@($c.comparisons|Select-Object -First 2)}
		'wrong-artifact-command'{$c.comparisons[2].candidateCommand.commandSerial=777}
		'unavailable-command-claimed-as-artifact'{$c.comparisons[2].cohortOrdinal=1}
		'missing-editor-serialization'{$c.candidateFinalNormalOutput.snapshotDiagnostics.fallbackEvents=@();$h.Candidate.finalNormalOutput.snapshotDiagnostics.fallbackEvents=@()}
	}
	Reject "dynamic-$fault" {if($fault -ceq 'missing-editor-serialization'){RmsParityFinalOutput $c $h}else{RmsParityLightingCohort $c $h ([pscustomobject]@{cameraCount=16})}}
}
foreach($fault in @('wrong-capacity','missing-admitted-row','wrong-queued-count','under-admitted-count')){
	$c=Clone $stressComparison;$h=@{Baseline=(Clone $stressHosts.Baseline);Candidate=(Clone $stressHosts.Candidate)}
	switch($fault){
		'wrong-capacity'{$c.normalOutputCoverage.backendSnapshotCapacity=16;foreach($variant in @('Baseline','Candidate')){$h[$variant].normalOutputCoverage.backendSnapshotCapacity=16}}
		'missing-admitted-row'{$h.Candidate.normalCommandMetadata=@($h.Candidate.normalCommandMetadata|Select-Object -First 31)}
		'wrong-queued-count'{$c.normalOutputCoverage.normalQueuedCameraCommands=8;foreach($variant in @('Baseline','Candidate')){$h[$variant].normalOutputCoverage.normalQueuedCameraCommands=8}}
		'under-admitted-count'{$c.normalOutputCoverage.actualRecordedCameraCommands=7;foreach($variant in @('Baseline','Candidate')){$h[$variant].normalOutputCoverage.actualRecordedCameraCommands=7}}
	}
	Reject "stress-$fault" {RmsParityNormalCoverage $c $h ([pscustomobject]@{cameraCount=16})}
}
$referenceSource=Join-Path $testRoot 'reference-RenderSystem.cpp';[IO.File]::WriteAllText($referenceSource,'Synthetic source-flow protocol bytes')
$candidateSource=Join-Path $testRoot 'Engine/Renderer/RenderSystem.cpp';$null=[IO.Directory]::CreateDirectory((Split-Path $candidateSource));[IO.File]::WriteAllText($candidateSource,'Synthetic candidate source-flow protocol bytes')
$referenceBytes=[IO.File]::ReadAllBytes($referenceSource);$referenceHeader=[Text.Encoding]::UTF8.GetBytes('blob '+$referenceBytes.Length+[char]0);$referenceBlob=[Convert]::ToHexString([Security.Cryptography.SHA1]::HashData([byte[]]($referenceHeader+$referenceBytes))).ToLowerInvariant()
$additional=@();foreach($name in @('VulkanShadowValidationReadback.cpp','VulkanReadback.cpp','VulkanEditorRendering.cpp','VulkanContextImpl.h','VulkanFrameRendering.cpp')){
	$relative='Engine/Renderer/RHI/Vulkan/'+$name;$reference=Join-Path $testRoot ('reference-'+$name);$current=Join-Path $testRoot $relative;$null=[IO.Directory]::CreateDirectory((Split-Path $current))
	$text=switch($name){'VulkanContextImpl.h'{'static constexpr uint32_t MAX_RENDER_SNAPSHOTS_PER_FRAME = 8u;'}'VulkanFrameRendering.cpp'{'if (!impl->editorMode) {} SubmitGraphicsQueue(); WaitGraphicsQueueIdle(); renderSnapshotSlotsUsed[currentFrame].fill(false); RenderSnapshotFallbackResult::SerializedSafely;'}default{'Synthetic protocol native source bytes'}}
	[IO.File]::WriteAllText($reference,$text);[IO.File]::WriteAllText($current,$text);$bytes=[IO.File]::ReadAllBytes($reference);$header=[Text.Encoding]::UTF8.GetBytes('blob '+$bytes.Length+[char]0);$blob=[Convert]::ToHexString([Security.Cryptography.SHA1]::HashData([byte[]]($header+$bytes))).ToLowerInvariant();$additional+=,[pscustomobject]@{sourcePath=$relative;referenceGitBlob=$blob;referenceSource=(RmsParityPin $reference);candidateSource=(RmsParityPin $current)}
}
$archivePin=WriteModel archive.json ([pscustomobject]@{sourceRevision=('a'*40)})
$sourceProof=[pscustomobject]@{sourceRevision=('a'*40);referenceGitBlob=$referenceBlob;referenceSource=(RmsParityPin $referenceSource);candidateSource=(RmsParityPin $candidateSource);findings=@([pscustomobject]@{fact='Synthetic protocol source observation'});interpretation='Synthetic protocol only';backendSnapshotCapacity=8;additionalSources=$additional;referenceCompatibilityCapacityHeader=($additional|Where-Object {$_.sourcePath.EndsWith('VulkanContextImpl.h')}).referenceSource;provenance=[pscustomobject]@{referenceReceipt=$archivePin;generator=(RmsParityPin $script:RmsParityJoinerPath);gitExecutable=(RmsParityPin $referenceSource)}}
$sourceProofPin=WriteModel source-proof.json $sourceProof;$sourceComparison=[pscustomobject]@{rendererSourceProof=$sourceProofPin};$sourceHosts=@{Baseline=[pscustomobject]@{rendererSourceProof=$sourceProofPin;archiveReceipt=$archivePin};Candidate=[pscustomobject]@{rendererSourceProof=$sourceProofPin;archiveReceipt=$archivePin}}
Check 'synthetic-old-gitblob-and-current-queue-source-pins' {RmsParitySourceProof $sourceComparison $sourceHosts ('a'*40)}
Reject 'old-source-revision-changed' {RmsParitySourceProof $sourceComparison $sourceHosts ('b'*40)}
$wrongHosts=@{Baseline=(Clone $sourceHosts.Baseline);Candidate=(Clone $sourceHosts.Candidate)};$wrongHosts.Candidate.rendererSourceProof.sha256=('0'*64)
Reject 'host-queue-source-proof-not-comparator-proof' {RmsParitySourceProof $sourceComparison $wrongHosts ('a'*40)}
foreach($fault in @('missing-archive-provenance','different-archive-file-same-bytes','missing-generator-provenance','different-run-archive-file')){
	$proof=Clone $sourceProof;$runArchive=$archivePin
	switch($fault){
		'missing-archive-provenance'{$proof.provenance.PSObject.Properties.Remove('referenceReceipt')}
		'different-archive-file-same-bytes'{$proof.provenance.referenceReceipt=WriteModel alternate-source-archive.json ([pscustomobject]@{sourceRevision=('a'*40)})}
		'missing-generator-provenance'{$proof.provenance.PSObject.Properties.Remove('generator')}
		'different-run-archive-file'{$runArchive=WriteModel alternate-run-archive.json ([pscustomobject]@{sourceRevision=('a'*40)})}
	}
	$proofPin=WriteModel ($fault+'.json') $proof;$c=[pscustomobject]@{rendererSourceProof=$proofPin};$h=@{Baseline=[pscustomobject]@{rendererSourceProof=$proofPin;archiveReceipt=$archivePin};Candidate=[pscustomobject]@{rendererSourceProof=$proofPin;archiveReceipt=$archivePin}}
	Reject "archive-join-$fault" {RmsParitySourceProof $c $h ('a'*40) $runArchive}
}
$instantText='2026-10-15T08:55:08.1234567Z';$instantOffset=[DateTimeOffset]::ParseExact($instantText,'o',[Globalization.CultureInfo]::InvariantCulture)
Check 'typed-utc-datetime-preserves-exact-instant' {RmsParityRequire ((RmsParityInstant $instantOffset.UtcDateTime).UtcTicks -eq $instantOffset.UtcTicks) 'Typed UTC instant changed'}
Check 'typed-offset-datetime-preserves-exact-instant' {RmsParityRequire ((RmsParityInstant ($instantOffset.ToOffset([TimeSpan]::FromHours(8)))).UtcTicks -eq $instantOffset.UtcTicks) 'Typed offset instant changed'}
Check 'invariant-iso-string-preserves-exact-instant' {RmsParityRequire ((RmsParityInstant '2026-10-15T16:55:08.1234567+08:00').UtcTicks -eq $instantOffset.UtcTicks) 'ISO offset instant changed'}
Check 'typed-and-string-equal-command-boundary' {RmsParityCommandChronology ([pscustomobject]@{startedUtc=$instantOffset.UtcDateTime;completedUtc=$instantText})}
Reject 'malformed-command-timestamp' {RmsParityCommandChronology ([pscustomobject]@{startedUtc='not a timestamp';completedUtc=$instantText})}
Reject 'locale-display-string-is-not-iso-proof' {RmsParityInstant '10/15/2026 08:55:08'}
Reject 'invalid-iso-calendar-date' {RmsParityInstant '2026-13-15T08:55:08Z'}
Reject 'timezone-free-string-is-ambiguous' {RmsParityInstant '2026-10-15T08:55:08'}
Reject 'unspecified-typed-datetime-is-ambiguous' {RmsParityInstant ([DateTime]::SpecifyKind($instantOffset.UtcDateTime,[DateTimeKind]::Unspecified))}
Reject 'reversed-actual-command-timestamps' {RmsParityCommandChronology ([pscustomobject]@{startedUtc=$instantOffset;completedUtc=$instantOffset.AddTicks(-1)})}
$qualificationRoot=Join-Path $testRoot 'qualification-case';$qualificationProject=Join-Path $qualificationRoot 'project';$null=[IO.Directory]::CreateDirectory($qualificationProject)
$qualificationPaths=@('Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_493d9bc3.teanim','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_fad19fd5.teanim','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_9beae902.teskel','Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_b7d2176c.teskel','Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf','Build/Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat','ProjectSettings/RenderSettings.json','Assets/Scenes/Scene.tescene')
$qualifiedFiles=@();$initialInputs=@();$generatedHashes=[ordered]@{};$referenceRuntime=[ordered]@{};$qualifiedCompared=[ordered]@{};$qualifiedContents=@{}
foreach($relative in $qualificationPaths){
	$target=Join-Path $qualificationProject $relative;$source=Join-Path (Join-Path $qualificationRoot 'reference') $relative;foreach($path in @($target,$source)){$null=[IO.Directory]::CreateDirectory((Split-Path $path));[IO.File]::WriteAllText($path,('qualified:'+ $relative))};$qualifiedContents[$target]='qualified:'+ $relative;$qualifiedContents[$source]='qualified:'+ $relative
	$currentPin=RmsParityPin $target;$sourcePin=RmsParityPin $source;$referenceRuntime[$relative]=$sourcePin;$qualifiedCompared[$relative]=[pscustomobject]@{sha256=$currentPin.sha256;bytes=$currentPin.bytes}
	$before=$null
	if(!$relative.EndsWith('ImportedHeroIdle_493d9bc3.teanim') -and !$relative.EndsWith('ImportedHeroSkeleton_9beae902.teskel')){
		$oldText=$(if($relative -ceq 'Assets/Scenes/Scene.tescene'){$qualifiedContents[$target]}else{'before:'+ $relative});$oldBytes=[Text.Encoding]::UTF8.GetBytes($oldText);$before=[pscustomobject]@{path=$target;sha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($oldBytes)).ToLowerInvariant();bytes=$oldBytes.Length}
		if($relative -in @('Assets/Scenes/Scene.tescene','ProjectSettings/RenderSettings.json')){$generatedHashes[$relative]=$before.sha256}else{$initialInputs+=,[pscustomobject]@{projectPath=$relative;sha256=$before.sha256;sizeBytes=$before.bytes}}
		if($relative -ceq $qualificationPaths[0]){[IO.File]::WriteAllText((Join-Path $qualificationRoot 'qualification-original-package-manifest.json'),$oldText)}
	}
	$qualifiedFiles+=,[pscustomobject]@{relative=$relative;original=$before;source=$sourcePin;qualified=$currentPin}
}
$initialMaterialization=[pscustomobject]@{projectRoot=$qualificationProject;inputClosure=$initialInputs;generatedHashes=[pscustomobject]$generatedHashes}
$initialMaterializationPin=WriteModel qualification-case/materialization.json $initialMaterialization
$referenceCapture=WriteModel qualification-case/reference-capture.json ([pscustomobject]@{scope='Synthetic reference capture protocol'})
$referenceLifecycle=WriteModel qualification-case/reference-lifecycle.json ([pscustomobject]@{normalClose=$true})
$referenceClosure=WriteModel qualification-case/reference-closure.json ([pscustomobject]@{schemaVersion='runtime-memory-safety-post-import-closure-v1';status='ReferenceObserved';references=@([pscustomobject]@{lane='small-A1';capture=$referenceCapture;lifecycle=$referenceLifecycle;runtime=[pscustomobject]$referenceRuntime})})
$qualification=[pscustomobject]@{schemaVersion='runtime-memory-safety-qualified-runtime-v1';status='Qualified';policy='FullActualPostImportReferenceRuntimeClosure';scale='small';projectRoot=$qualificationProject;originalMaterialization=$initialMaterializationPin;referenceClosureReceipt=$referenceClosure;referenceCapture=$referenceCapture;referenceLifecycle=$referenceLifecycle;originalPackageManifest=(RmsParityPin (Join-Path $qualificationRoot 'qualification-original-package-manifest.json'));files=$qualifiedFiles}
$qualificationComparison=[pscustomobject]@{scale='small';qualificationSource=$referenceClosure;qualifiedActualPostImportRuntimeClosure=[pscustomobject]$qualifiedCompared}
Check 'historical-before-overwrite-and-live-qualified-runtime-are-distinct' {
	$script:parityPins=@{};$transition=RmsParityQualification $qualification $initialMaterialization $qualificationComparison
	$before=$transition.transitions[0].historicalBefore
	RmsParityRequire ($transition.transitions.Count -eq 9 -and $before.isLivePathPin -eq $false -and $before.projectRelativePath -ceq $qualifiedFiles[0].relative -and $before.observedBeforeSha256 -ceq $qualifiedFiles[0].original.sha256 -and $before.observedBeforeBytes -eq $qualifiedFiles[0].original.bytes -and !$before.Contains('path') -and !$before.Contains('sha256') -and !$before.Contains('bytes') -and $script:parityPins[$qualifiedFiles[0].qualified.path].sha256 -ceq $qualifiedFiles[0].qualified.sha256 -and $script:parityPins[$qualifiedFiles[0].qualified.path].sha256 -cne $qualifiedFiles[0].original.sha256) 'Historical before-fact entered live pin set'
}
foreach($fault in @('changed-qualified-bytes','changed-reference-source','forged-old-manifest','forged-materialization-hash','forged-before-path','omitted-qualified-path','duplicate-qualified-path','missing-manifest-backup','altered-before-bytecount','false-before-absence','qualified-path-outside-project','comparison-qualified-hash','source-path-same-bytes-different-file')){
	$script:parityPins=@{};$q=Clone $qualification;$m=Clone $initialMaterialization;$c=Clone $qualificationComparison;$backupBytes=$null
	switch($fault){
		'changed-qualified-bytes'{[IO.File]::WriteAllText($q.files[0].qualified.path,'unexpected qualified mutation')}
		'changed-reference-source'{[IO.File]::WriteAllText($q.files[0].source.path,'unexpected source mutation')}
		'forged-old-manifest'{$q.files[0].original.sha256=('0'*64)}
		'forged-materialization-hash'{$m.inputClosure[0].sha256=('0'*64)}
		'forged-before-path'{$q.files[0].original.path=$q.files[0].source.path}
		'omitted-qualified-path'{$q.files=@($q.files|Select-Object -First 8)}
		'duplicate-qualified-path'{$q.files[8]=Clone $q.files[0]}
		'missing-manifest-backup'{$backupBytes=[IO.File]::ReadAllBytes($q.originalPackageManifest.path);[IO.File]::Delete($q.originalPackageManifest.path)}
		'altered-before-bytecount'{$q.files[0].original.bytes++}
		'false-before-absence'{$q.files[0].original=$null}
		'qualified-path-outside-project'{$q.files[0].qualified.path=$q.files[0].source.path}
		'comparison-qualified-hash'{$c.qualifiedActualPostImportRuntimeClosure.PSObject.Properties[$qualificationPaths[0]].Value.sha256=('0'*64)}
		'source-path-same-bytes-different-file'{$q.files[0].source=$q.files[0].qualified}
	}
	try{Reject "qualification-$fault" {RmsParityQualification $q $m $c}}finally{foreach($path in $qualifiedContents.Keys){[IO.File]::WriteAllText($path,$qualifiedContents[$path])};if($null -ne $backupBytes){[IO.File]::WriteAllBytes($qualification.originalPackageManifest.path,$backupBytes)}}
}
$script:parityPins=@{}
Reject 'generic-runtime-binary-original-pin-still-strict' {RmsParityWalk ([pscustomobject]@{runtimeClosure=[pscustomobject]@{matchingClosure=@([pscustomobject]@{original=$qualification.files[0].original;staged=$qualification.files[0].qualified})}})}
$report=@{status='ProtocolChecksPassed';caseCount=$script:checks;nativeAcceptance=$false;performanceAcceptance=$false;actualParityJoinExecuted=$false;scope='Synthetic protocol helper checks only; no native process or full actual join'}
[IO.File]::WriteAllText((Join-Path $testRoot 'result.json'),($report|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
Write-Output "$($script:checks) parity join protocol checks passed; no actual parity acceptance"
