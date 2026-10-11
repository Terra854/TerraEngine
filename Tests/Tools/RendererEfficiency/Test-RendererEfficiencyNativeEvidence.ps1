<#
.SYNOPSIS
Exercises the real comparer native-proof functions using small synthetic CPU files.
.DESCRIPTION
Run the installed script with PowerShell -NoProfile -File and a fresh -WorkRoot.
These serializer/verifier unit cases do not claim any GPU acceptance or timed run.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WorkRoot,[string]$Comparer='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
if(!$Comparer){$Comparer=Join-Path $PSScriptRoot 'Compare-RendererEfficiencyEvidence.ps1'}
$root=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $root){throw 'Native verifier tests require a fresh WorkRoot'}
[IO.Directory]::CreateDirectory($root)|Out-Null
# Load exact production function AST bodies without executing the comparer CLI.
# Every assertion below calls those functions, rather than duplicating their rules.
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($Comparer),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Comparer syntax error'}
foreach($node in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){Invoke-Expression $node.Extent.Text}
InitializeNativeProof
$utf8=[Text.UTF8Encoding]::new($false)
function Wrap($value){return [pscustomobject]@{status='Valid';value=$value;reason=''}}
function Clone($value){return ConvertTo-Json -InputObject $value -Depth 100 -Compress|ConvertFrom-Json}
$identity=[pscustomobject]@{fixtureId='shadow-invalidation';fixtureHash=(Wrap ('a'*64));runId='native-unit';runIndex=0;seed=65001;mode='Candidate';stage='P1';resolution=[pscustomobject]@{width=7;height=5}}
$origin=[pscustomobject]@{cell=@('0','0','0');offsetMetres=@(0,0,0)}
$matrix=@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)
$key=[pscustomobject]@{rendererSessionGeneration='4';viewType='Scene';viewId=1;lightId=('1'*32);faceIndex=0;resourceKind=2;allocationGeneration='1';regionId='0';extent=@(2,1);depthFormat=126;referenceOrigin=$origin;lightProjectionStamp=[pscustomobject]@{viewProjection=$matrix;lightParameters=@(0,0,0,0);shadowFilterParameters=@(0,0,0,0);cubeProjectionEvidenceComplete=$true;cubeViewProjections=@($matrix,$matrix,$matrix,$matrix,$matrix,$matrix)}}
$caster=[ordered]@{objectId=('2'*32);lifetimeGeneration='1';segmentId='0';worldPosition=$origin;referenceOrigin=$origin;rotation=@(1,0,0,0);scale=@(1,1,1);materialShadowParameters=@(0,0,0,0);texturePublications=@();deformationPalette=@();morphWeights=@();geometryRoute=0;selectedLod=0;selectionFlags=0;payloadSource=0;emittedSource=0;staticEligible=$true;staticClassificationReason=0;evidenceMask='511'}
foreach($field in @('meshId','assetId','materialId','shaderId','textureId')){$caster[$field]='3'*32}
foreach($field in @('firstMeshlet','meshletCount','firstCluster','clusterCount','pageId','fallbackPageId','payloadByteOffset','payloadByteSize','shadowBehavior')){$caster[$field]='0'}
$counters=[pscustomobject]@{schemaVersion=1;fixtureId=$identity.fixtureId;fixtureHash=('a'*64);operationId=('4'*32);fixtureStepId='shadow-invalidation-00';runId=$identity.runId;runIndex=0;seed=65001;mode='Candidate';stage='P1';measurementPhase='Measured';stepFrame='0';qualifiedShadowViewKey=$key;sourceContentOwner=[pscustomobject]@{nativeOwner='1';qualifiedShadowViewKey=$key;contentToken='1'};contentToken='1';depthSourceFormat=126;depthNativeExtent=@(2,1);depthEncoding='float32-le';coverageEncoding='occupied-u8';colorSourceFormat=44;colorNativeExtent=@(7,5);imageEncoding='png-rgba8';readbackProvenance=[pscustomobject]@{kind='FullNativeSameCommandCompletedReadback';frameSerial='5';commandSerial='6';submitSerial='7';completionSerial='7';rendererSessionGeneration='4';settingsGeneration='2';mutationGeneration='3'};eligibleCasterIdentityRows=@([pscustomobject]$caster);submittedCasterIdentityRows=@([pscustomobject]$caster);nativeCounters=[pscustomobject]@{eligibleCasterSegments=(Wrap 1);submittedCasterSegments=(Wrap 1);actualDrawSubmissions=(Wrap 1);shadowReuses=(Wrap 0)};effectiveOptions=[pscustomobject]@{shadowReuse='Enabled'}}
$frame=[pscustomobject]@{fixtureStepId=$counters.fixtureStepId;measurementPhase='Measured';stepFrame=0;viewType='Scene';viewId=1;lightId=(Wrap ('1'*32));faceIndex=(Wrap 0);frameSerial=(Wrap 5);commandSerial=(Wrap 6);submitSerial=(Wrap 7);completionSerial=(Wrap 7);rendererSessionGeneration=(Wrap 4);settingsGeneration=(Wrap 2);mutationGeneration=(Wrap 3);publicationState='Complete';effectiveOptions=$counters.effectiveOptions;shadow=$counters.nativeCounters;artifacts=@()}
$depth=[byte[]](@([BitConverter]::GetBytes([single]0.5))+@([BitConverter]::GetBytes([single]1)))
$coverage=[byte[]]@(1,0)
Add-Type -AssemblyName System.Drawing
$bitmap=[Drawing.Bitmap]::new(7,5);$stream=[IO.MemoryStream]::new()
try{$bitmap.SetPixel(0,0,[Drawing.Color]::FromArgb(255,30,20,10));$bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png);$image=$stream.ToArray()}finally{$bitmap.Dispose();$stream.Dispose()}
$originalFrame=Clone $frame;$originalCounters=Clone $counters
function SaveProof($f,$count,$d=$depth,$cover=$coverage,$png=$image){
	# Schema names the frame count reuseRegionCount; native sidecar names the
	# same actual typed counter shadowReuses. This is a serialization mapping.
	if(!(Has $f.shadow 'reuseRegionCount')){$f.shadow|Add-Member reuseRegionCount (Clone $f.shadow.shadowReuses)}
	$artifacts=[Collections.Generic.List[object]]::new()
	foreach($entry in @(@('Depth','depth.f32',$d),@('Coverage','coverage.bin',$cover),@('Image','image.png',$png),@('Counters','counters.json',$utf8.GetBytes((ConvertTo-Json -InputObject $count -Depth 100 -Compress))))){
		$path=Join-Path $root $entry[1];[IO.File]::WriteAllBytes($path,[byte[]]$entry[2]);$artifacts.Add([pscustomobject]@{kind=$entry[0];status='Valid';path=$entry[1];sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();reason=''})
	}
	$f.artifacts=$artifacts.ToArray();return ReadNativeProof $f $identity $root
}
$passed=0
function Case([string]$Name,[scriptblock]$Body,[bool]$Reject=$false,[string]$Expected='NATIVE_FAILED:'){
	try{& $Body|Out-Null;if($Reject){throw 'Expected native proof rejection'};Write-Output "PASS $Name";$script:passed++}
	catch{if(!$Reject -or !$_.Exception.Message.Contains($Expected)){throw};Write-Output "PASS $Name rejected";$script:passed++}
}
Case 'valid complete native CPU proof' {$proof=SaveProof (Clone $originalFrame) (Clone $originalCounters);$null=CompareNativeProof $proof $proof $identity.resolution}
Case 'forged counter count' {$c=Clone $originalCounters;$c.nativeCounters.actualDrawSubmissions.value=99;SaveProof (Clone $originalFrame) $c} $true
Case 'forged completion receipt' {$c=Clone $originalCounters;$c.readbackProvenance.completionSerial='6';SaveProof (Clone $originalFrame) $c} $true
Case 'source owner cannot name a different physical content token' {$c=Clone $originalCounters;$c.sourceContentOwner.nativeOwner='99';SaveProof (Clone $originalFrame) $c} $true
Case 'forged native depth extent' {$c=Clone $originalCounters;$c.depthNativeExtent=@(1,1);$c.qualifiedShadowViewKey.extent=@(1,1);$c.sourceContentOwner.qualifiedShadowViewKey.extent=@(1,1);SaveProof (Clone $originalFrame) $c} $true
Case 'inconsistent coverage bytes' {SaveProof (Clone $originalFrame) (Clone $originalCounters) $depth ([byte[]]@(0,0))} $true
Case 'nonfinite D32 bytes' {$bad=[byte[]](@([BitConverter]::GetBytes([single]::NaN))+@([BitConverter]::GetBytes([single]1)));SaveProof (Clone $originalFrame) (Clone $originalCounters) $bad} $true
Case 'same SHA unknown native encoding remains unverified' {$c=Clone $originalCounters;$c.depthEncoding='unknown';SaveProof (Clone $originalFrame) $c} $true 'UNVERIFIED:'
Case 'wrong actual PNG camera extent' {$c=Clone $originalCounters;$c.colorNativeExtent=@(8,5);SaveProof (Clone $originalFrame) $c} $true
Case 'actual PNG dimensions cannot hide behind matching declarations' {$png=New-Object Drawing.Bitmap 8,5;$memory=[IO.MemoryStream]::new();try{$png.Save($memory,[Drawing.Imaging.ImageFormat]::Png);$bad=$memory.ToArray()}finally{$png.Dispose();$memory.Dispose()};SaveProof (Clone $originalFrame) (Clone $originalCounters) $depth $coverage $bad} $true
Case 'duplicate exact caster row' {$c=Clone $originalCounters;$c.eligibleCasterIdentityRows=@($c.eligibleCasterIdentityRows[0],$c.eligibleCasterIdentityRows[0]);SaveProof (Clone $originalFrame) $c} $true
Case 'reference cannot claim reused empty submission' {$c=Clone $originalCounters;$f=Clone $originalFrame;$c.effectiveOptions.shadowReuse='AlwaysRefresh';$f.effectiveOptions.shadowReuse='AlwaysRefresh';$c.submittedCasterIdentityRows=@();$c.nativeCounters.submittedCasterSegments.value=0;$c.nativeCounters.actualDrawSubmissions.value=0;$c.nativeCounters.shadowReuses.value=1;$f.shadow=Clone $c.nativeCounters;SaveProof $f $c} $true
Case 'nonempty refreshed caster set requires an actual draw submission' {$c=Clone $originalCounters;$f=Clone $originalFrame;$c.nativeCounters.actualDrawSubmissions.value=0;$f.shadow=Clone $c.nativeCounters;SaveProof $f $c} $true
Case 'valid reused native content has no submitted rows or draws' {$c=Clone $originalCounters;$f=Clone $originalFrame;$c.submittedCasterIdentityRows=@();$c.nativeCounters.submittedCasterSegments.value=0;$c.nativeCounters.actualDrawSubmissions.value=0;$c.nativeCounters.shadowReuses.value=1;$f.shadow=Clone $c.nativeCounters;SaveProof $f $c}
Case 'changed selected caster geometry detected independently' {$a=SaveProof (Clone $originalFrame) (Clone $originalCounters);$c=Clone $originalCounters;$c.eligibleCasterIdentityRows[0].selectedLod=1;$c.submittedCasterIdentityRows[0].selectedLod=1;$b=SaveProof (Clone $originalFrame) $c;CompareNativeProof $a $b $identity.resolution} $true
Case 'independent import epochs preserve semantic identity comparison' {$a=SaveProof (Clone $originalFrame) (Clone $originalCounters);$c=Clone $originalCounters;$c.eligibleCasterIdentityRows[0].lifetimeGeneration='99';$c.submittedCasterIdentityRows[0].lifetimeGeneration='99';$c.eligibleCasterIdentityRows[0]|Add-Member meshRevision '200';$c.submittedCasterIdentityRows[0]|Add-Member meshRevision '200';$b=SaveProof (Clone $originalFrame) $c;$null=CompareNativeProof $a $b $identity.resolution}
Case 'depth error above frozen tolerance rejected independently' {$a=SaveProof (Clone $originalFrame) (Clone $originalCounters);$d=[byte[]](@([BitConverter]::GetBytes([single]0.50002))+@([BitConverter]::GetBytes([single]1)));$b=SaveProof (Clone $originalFrame) (Clone $originalCounters) $d;CompareNativeProof $a $b $identity.resolution} $true
Case 'incomplete semantic membership stays unverified' {$c=Clone $originalCounters;$c.eligibleCasterIdentityRows[0].evidenceMask='1';$c.submittedCasterIdentityRows[0].evidenceMask='1';SaveProof (Clone $originalFrame) $c} $true 'UNVERIFIED:'
Case 'all-empty native outputs cannot pass fixture correctness' {VerifyNativeStepCoverage 0 0} $true
Case 'partial six-face native point cube rejected' {$c=Clone $originalCounters;$c.qualifiedShadowViewKey.resourceKind=1;VerifyNativeBundle @($c)} $true
Case 'texture publication epochs differ but exact bindings match' {$a=Clone $originalCounters;$b=Clone $originalCounters;$a.eligibleCasterIdentityRows[0].texturePublications=@([pscustomobject]@{textureId=('5'*32);publicationGeneration='4';bindingIndex=0});$b.eligibleCasterIdentityRows[0].texturePublications=@([pscustomobject]@{textureId=('5'*32);publicationGeneration='9';bindingIndex=0});Require (((NativeCasterRows $a.eligibleCasterIdentityRows $true) -join "`n") -ceq ((NativeCasterRows $b.eligibleCasterIdentityRows $true) -join "`n")) 'Exact texture bindings changed'}
Case 'missing texture publication remains unverified' {$c=Clone $originalCounters;$c.eligibleCasterIdentityRows[0].PSObject.Properties.Remove('texturePublications');NativeCasterRows $c.eligibleCasterIdentityRows} $true 'UNVERIFIED:'
Case 'zero texture publication rejects semantic proof' {$c=Clone $originalCounters;$c.eligibleCasterIdentityRows[0].texturePublications=@([pscustomobject]@{textureId=('5'*32);publicationGeneration='0';bindingIndex=0});NativeCasterRows $c.eligibleCasterIdentityRows} $true
# Receipt fixtures below are synthetic CPU serialization tests only. They call
# production guards and cannot claim actual Vulkan, scheduler, or scene results.
$steps=@([pscustomobject]@{id='recovery-00';action='recorded-submit-abort'},[pscustomobject]@{id='recovery-01';action='incomplete-point-faces'},[pscustomobject]@{id='recovery-02';action='resource-recreation'},[pscustomobject]@{id='recovery-03';action='device-recovery'},[pscustomobject]@{id='recovery-13';action='transaction-conflict-rollback'})
$envelope=[pscustomobject]@{schemaVersion=1;operationId=('4'*32);fixtureId=$identity.fixtureId;fixtureHash=('a'*64);stage=$identity.stage;runId=$identity.runId;runIndex=0;mode=$identity.mode;seed=$identity.seed;receipts=@()}
function Action([string]$Injection,[string]$Step){return [pscustomobject]@{kind='FixtureAction';fixtureStepId=$Step;injection=$Injection;correlation=[pscustomobject]@{operationId=$envelope.operationId;fixtureStepId=$Step;stepFrame='361';mutationGeneration='3';settingsGeneration='2';rendererSessionGeneration='4'};frameSerial='5';commandSerial='6';submitSerial='7';completionSerial='7';oldAllocationGeneration='1';newAllocationGeneration='1';requiredFaceMask=63;recordedFaceMask=31;recorded=$true;submitted=$true;completed=$true;priorOwnerRejected=$true;reason='CPU receipt fixture'}}
$abort=Action RecordedSubmitAbort recovery-00;$abort.submitSerial='0';$abort.completionSerial='0';$abort.submitted=$false;$abort.completed=$false
$partial=Action IncompletePointFaces recovery-01
$resource=Action ShadowResourceRecreation recovery-02;$resource.newAllocationGeneration='2'
$before=[pscustomobject]@{objectId=('2'*32);lifetimeGeneration='1';metadataGeneration='1';componentGeneration='1';hierarchyRevision='1';transformRevision='1';coordinateFrameRevision='1';spatialRevision='1';parentObjectId=('0'*32);active=$true;canonicalWorldPose=[pscustomobject]@{position=$origin;rotation=@(1,0,0,0);scale=@(1,1,1)}}
# A real rejected queue receipt has no new spatial publication. Preserve its
# zero instead of inventing an epoch; exact target revisions prove rollback.
# The frozen native recipe contains exactly DeleteObject and SetWorldTransform.
# Its real queue result serializes Scene::ToString(Conflicted), not a fabricated
# generic rejection. Both writes must remain unapplied and rejected together.
$rollback=[pscustomobject]@{kind='TransactionRollback';fixtureStepId='recovery-13';transactionId='native-transaction';status='conflicted';appliedWriteCount=0;rejectedWriteCount=2;spatialPublicationGeneration='0';before=$before;after=(Clone $before);message='Transaction rejected before commit'}
$device=[pscustomobject]@{kind='DeviceRecreation';fixtureStepId='recovery-03';status='Completed';idleProven=$true;rebound=$true;oldRendererSessionGeneration='4';newRendererSessionGeneration='5';oldBackendResourceGeneration='7';newBackendResourceGeneration='8';priorOptionsEpoch='2';newOptionsEpoch='3';visibleSettingsGeneration='4';request=[pscustomobject]@{operationId=$envelope.operationId;fixtureStepId='recovery-03';fixtureId=$envelope.fixtureId;fixtureHash=$envelope.fixtureHash;projectGeneration='1';expectedRendererSessionGeneration='4';expectedBackendResourceGeneration='7'};reason=''}
$envelope.receipts=@($abort,$partial,$resource,$device,$rollback)
function SaveReceipts($proof){$path=Join-Path $root 'native.receipts.json';[IO.File]::WriteAllText($path,(Canonical $proof),$utf8);$document=[pscustomobject]@{artifacts=@([pscustomobject]@{kind='Counters';status='Valid';path='native.receipts.json';sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();reason=''})};return ReadNativeValidationReceipts $document $identity $root $envelope.operationId}
Case 'complete CPU five-receipt envelope accepted' {$p=SaveReceipts (Clone $envelope);VerifyNativeRecoveryReceipts $p $steps}
Case 'missing actual action cannot be producer-passed' {$p=Clone $envelope;$p.receipts=@($p.receipts|Where-Object {!(Has $_ 'injection') -or $_.injection -ne 'IncompletePointFaces'});VerifyNativeRecoveryReceipts $p $steps} $true 'UNVERIFIED:'
Case 'hashed receipt wrong run identity rejected' {$p=Clone $envelope;$p.runIndex=9;SaveReceipts $p} $true
Case 'recorded abort cannot claim a submit' {$p=Clone $envelope;$p.receipts[0].submitSerial='7';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'partial cube cannot claim all faces recorded' {$p=Clone $envelope;$p.receipts[1].recordedFaceMask=63;VerifyNativeRecoveryReceipts $p $steps} $true
Case 'partial cube completion requires submit receipt' {$p=Clone $envelope;$p.receipts[1].submitSerial='0';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'partial cube must reject prior owner' {$p=Clone $envelope;$p.receipts[1].priorOwnerRejected=$false;VerifyNativeRecoveryReceipts $p $steps} $true
Case 'resource recreation cannot reuse allocation epoch' {$p=Clone $envelope;$p.receipts[2].newAllocationGeneration='1';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'device recreation requires fresh native backend and UI rebind' {$p=Clone $envelope;$p.receipts[3].rebound=$false;VerifyNativeRecoveryReceipts $p $steps} $true
Case 'device recreation requires actual requested old generation' {$p=Clone $envelope;$p.receipts[3].request.expectedRendererSessionGeneration='3';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'typed rollback cannot apply one partial write' {$p=Clone $envelope;$p.receipts[4].appliedWriteCount=1;VerifyNativeRecoveryReceipts $p $steps} $true
Case 'typed frozen conflict cannot synthesize generic Rejected status' {$p=Clone $envelope;$p.receipts[4].status='Rejected';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'typed frozen conflict cannot claim Committed status' {$p=Clone $envelope;$p.receipts[4].status='Committed';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'typed frozen conflict cannot reject only one of two writes' {$p=Clone $envelope;$p.receipts[4].rejectedWriteCount=1;VerifyNativeRecoveryReceipts $p $steps} $true
Case 'typed frozen conflict cannot invent a third rejected write' {$p=Clone $envelope;$p.receipts[4].rejectedWriteCount=3;VerifyNativeRecoveryReceipts $p $steps} $true
Case 'typed rollback must preserve exact before after pose' {$p=Clone $envelope;$p.receipts[4].after.canonicalWorldPose.position.offsetMetres[0]=1;VerifyNativeRecoveryReceipts $p $steps} $true
Case 'typed rollback must preserve actual target spatial revision' {$p=Clone $envelope;$p.receipts[4].after.spatialRevision='2';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'zero target spatial revision cannot prove rollback' {$p=Clone $envelope;$p.receipts[4].before.spatialRevision='0';$p.receipts[4].after.spatialRevision='0';VerifyNativeRecoveryReceipts $p $steps} $true
Case 'missing target spatial revision remains unverified' {$p=Clone $envelope;$p.receipts[4].before.PSObject.Properties.Remove('spatialRevision');VerifyNativeRecoveryReceipts $p $steps} $true 'UNVERIFIED:'
Case 'missing before-state remains unverified' {$p=Clone $envelope;$p.receipts[4].PSObject.Properties.Remove('before');VerifyNativeRecoveryReceipts $p $steps} $true 'UNVERIFIED:'
Case 'serial actual task receipt requires inline execution' {VerifyNativePreparation ([pscustomobject]@{nativePreparation=[pscustomobject]@{workerTasks=0;inlineTasks=4;settled=$true}}) Serial}
Case 'concurrent intent cannot pass with inline fallback only' {VerifyNativePreparation ([pscustomobject]@{nativePreparation=[pscustomobject]@{workerTasks=0;inlineTasks=4;settled=$true}}) Concurrent} $true
Case 'unsettled worker tasks cannot prove native correctness' {VerifyNativePreparation ([pscustomobject]@{nativePreparation=[pscustomobject]@{workerTasks=4;inlineTasks=0;settled=$false}}) Concurrent} $true
function Workload([string]$Owner,[string]$Phase,[bool]$Worker){return [pscustomobject]@{name='copied-workload';ownerSystem=$Owner;phase=$Phase;reads=@(if($Worker){'RendererFrameSnapshot'}else{'RendererEfficiencyOptionsQueue'});writes=@(if($Worker){'RendererWorkPackets'}else{'RendererEfficiencyOptions';'RendererEfficiencyEvidence'});dependencies=@();workerEligible=$Worker;mainThreadRequired=(!$Worker);phaseKind=$(if($Worker){'worker-prepare'}else{'protected-commit'});boundaryClassification=$(if($Worker){'worker-safe'}else{'protected'});mutationPolicy='deferred';protectedCommit='actual protected commit';protectedReason='actual ownership boundary';safeSubwork='immutable task inputs'}}
$scheduler=[pscustomobject]@{kind='Scheduler';frameId='5';valid=$true;completed=$true;executionMode='parallel';parallelExecutionUsed=$true;preparationOption='Concurrent';systems=@('RenderSystem','EditorSystem');phases=@('Render','PostFrame');flushPoints=@('Frame End (frame-end)');mainThreadSystems=@('EditorSystem');workerCategories=@('renderer');protectedCategories=@('presentation');workloads=@((Workload RenderSystem Render $true),(Workload RenderSystem Render $false),(Workload EditorSystem PostFrame $false))}
$scheduled=Clone $envelope;$scheduled.receipts=@($scheduler)
Case 'copied CPU scheduler declarations verified' {VerifyNativeSchedulerReceipt $scheduled Concurrent}
Case 'scheduler missing protected contract stays unverified' {$p=Clone $scheduled;$p.receipts[0].workloads[1].PSObject.Properties.Remove('protectedReason');VerifyNativeSchedulerReceipt $p Concurrent} $true 'UNVERIFIED:'
Case 'scheduler cannot fabricate completed published frame' {$p=Clone $scheduled;$p.receipts[0].completed=$false;VerifyNativeSchedulerReceipt $p Concurrent} $true
Case 'scheduler intent cannot silently change preparation' {$p=Clone $scheduled;$p.receipts[0].preparationOption='Serial';VerifyNativeSchedulerReceipt $p Concurrent} $true
Case 'scheduler read/write declarations cannot disappear' {$p=Clone $scheduled;foreach($w in $p.receipts[0].workloads){$w.writes=@()};VerifyNativeSchedulerReceipt $p Concurrent} $true
Case 'worker cannot manufacture protected live-state writes' {$p=Clone $scheduled;$p.receipts[0].workloads[0].writes=@('ObjectManager');VerifyNativeSchedulerReceipt $p Concurrent} $true
$options=[pscustomobject]@{shadowReuse='Enabled';lightingUploads='Dirty';lightingDepth='Cooperative';shadowSubmission='Grouped';staticShadowCacheBudgetBytes=67108864;validationPreparation='Concurrent'}
$lifecycle=[pscustomobject]@{kind='RendererEfficiencyRunnerLifecycle';schemaVersion=1;fixtureId=$identity.fixtureId;fixtureHash=('a'*64);runId=$identity.runId;runIndex=0;preparation='Concurrent';operationId=$envelope.operationId;optionsRestored=$true;ownedEditorProcessId=123;normalClose=$true;processExitCode=0;watchdogFailed=$false;originalOptions=$options;restoredOptions=(Clone $options);restoredSettingsGeneration='4';restoredRendererSessionGeneration='5'}
function SaveLifecycle($proof){$path=Join-Path $root 'native.lifecycle.json';[IO.File]::WriteAllText($path,(Canonical $proof),$utf8);$doc=[pscustomobject]@{artifacts=@([pscustomobject]@{kind='Counters';status='Valid';path='native.lifecycle.json';sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();reason=''})};return ReadNativeRunnerLifecycle $doc $identity $root $envelope.operationId Concurrent}
Case 'owned CPU lifecycle restoration receipt verified' {$null=SaveLifecycle (Clone $lifecycle)}
Case 'forced watchdog close cannot claim normal lifecycle' {$p=Clone $lifecycle;$p.watchdogFailed=$true;SaveLifecycle $p} $true
Case 'restored option mismatch cannot claim cleanup passed' {$p=Clone $lifecycle;$p.restoredOptions.shadowReuse='AlwaysRefresh';SaveLifecycle $p} $true
Case 'missing actual close receipt stays unverified' {$p=Clone $lifecycle;$p.PSObject.Properties.Remove('normalClose');SaveLifecycle $p} $true 'UNVERIFIED:'
$diagnosticFrame=Clone $originalFrame;$diagnosticFrame.shadow|Add-Member reuseRegionCount (Wrap 0);$diagnosticFrame.shadow|Add-Member refreshReason (Wrap 'ContentOrOwnerChanged');$diagnosticFrame.shadow|Add-Member contentOwner (Wrap '1');$diagnosticFrame.shadow|Add-Member fallbackReason ([pscustomobject]@{status='Unavailable';value=$null;reason='Native text was not published.'})
$diagnosticCounters=Clone $originalCounters;$diagnosticCounters|Add-Member nativeShadowDiagnostics ([pscustomobject]@{refreshReason='ContentOrOwnerChanged';fallbackReason='';contentOwner='1'})
Case 'native refresh diagnostic agrees with content owner' {VerifyNativeShadowDiagnostics $diagnosticFrame $diagnosticCounters}
Case 'forged root refresh diagnostic rejected against sidecar' {$f=Clone $diagnosticFrame;$f.shadow.refreshReason.value='AlwaysRefresh';VerifyNativeShadowDiagnostics $f $diagnosticCounters} $true
Case 'missing native diagnostics cannot producer-pass SC009' {$p=Clone $diagnosticCounters;$p.PSObject.Properties.Remove('nativeShadowDiagnostics');VerifyNativeShadowDiagnostics $diagnosticFrame $p} $true 'UNVERIFIED:'
# Resource proof tests feed exact production functions detached CPU receipts and
# caster owners. A producer acceptance flag without a matching row cannot pass.
$resourceStep=[pscustomobject]@{id='shadow-invalidation-06';action='mesh-revision'}
$resource=[pscustomobject]@{kind='ResourcePublication';fixtureStepId=$resourceStep.id;action=$resourceStep.action;assetId=$caster.meshId;assetType='Mesh';sourcePath='Assets/Models/caster.gltf';sourceSha256=('a'*64);baselineSourceSha256=('b'*64);publicationGeneration=12;previousPublicationGeneration=11;actualNativeOwnerAccepted=$true}
$resourceEnvelope=[pscustomobject]@{receipts=@($resource)}
$resourceCounter=Clone $originalCounters;$resourceCounter.fixtureStepId=$resourceStep.id;$resourceCounter.eligibleCasterIdentityRows[0]|Add-Member meshRevision '12' -Force
Case 'native resource receipt reaches actual captured mesh owner' {$null=VerifyNativeResourcePublications $resourceEnvelope @($resourceStep) @($resourceCounter)}
Case 'resource flag cannot replace missing native receipt' {$p=Clone $resourceEnvelope;$p.receipts=@();VerifyNativeResourcePublications $p @($resourceStep) @($resourceCounter)} $true 'UNVERIFIED:'
Case 'malformed resource UUID rejected without truncation' {$p=Clone $resourceEnvelope;$p.receipts[0].assetId='12';VerifyNativeResourcePublications $p @($resourceStep) @($resourceCounter)} $true
Case 'unchanged publication cannot claim a new native owner' {$p=Clone $resourceEnvelope;$p.receipts[0].publicationGeneration=11;VerifyNativeResourcePublications $p @($resourceStep) @($resourceCounter)} $true
Case 'resource source must differ from baseline bytes' {$p=Clone $resourceEnvelope;$p.receipts[0].sourceSha256=$p.receipts[0].baselineSourceSha256;VerifyNativeResourcePublications $p @($resourceStep) @($resourceCounter)} $true
Case 'resource source path cannot escape project Assets' {$p=Clone $resourceEnvelope;$p.receipts[0].sourcePath='Assets/../caster.gltf';VerifyNativeResourcePublications $p @($resourceStep) @($resourceCounter)} $true
Case 'producer acceptance cannot hide old captured owner' {$p=Clone $resourceCounter;$p.eligibleCasterIdentityRows[0].meshRevision='11';VerifyNativeResourcePublications $resourceEnvelope @($resourceStep) @($p)} $true
Case 'unobserved resource owner remains unverified' {$p=Clone $resourceCounter;$p.eligibleCasterIdentityRows[0].meshId=('e'*32);VerifyNativeResourcePublications $resourceEnvelope @($resourceStep) @($p)} $true 'UNVERIFIED:'
Case 'matched resource bytes allow independent import epochs' {$p=Clone $resourceEnvelope;$p.receipts[0].publicationGeneration=22;$p.receipts[0].previousPublicationGeneration=21;$c=Clone $resourceCounter;$c.eligibleCasterIdentityRows[0].meshRevision='22';CompareNativeResourcePublications $p $resourceEnvelope @($resourceStep) @($c) @($resourceCounter)}
Case 'reference candidate changed source hashes must match' {$p=Clone $resourceEnvelope;$p.receipts[0].sourceSha256=('c'*64);CompareNativeResourcePublications $p $resourceEnvelope @($resourceStep) @($resourceCounter) @($resourceCounter)} $true
Case 'texture receipt requires its exact captured binding owner' {$s=Clone $resourceStep;$s.action='texture-revision';$p=Clone $resourceEnvelope;$p.receipts[0].action=$s.action;$p.receipts[0].assetType='Texture';$p.receipts[0].assetId=('d'*32);$c=Clone $resourceCounter;$c.eligibleCasterIdentityRows[0].texturePublications=@([pscustomobject]@{textureId=('d'*32);bindingIndex=3;publicationGeneration='12'});$null=VerifyNativeResourcePublications $p @($s) @($c)}
Case 'texture native owner cannot be old despite acceptance flag' {$s=Clone $resourceStep;$s.action='texture-revision';$p=Clone $resourceEnvelope;$p.receipts[0].action=$s.action;$p.receipts[0].assetType='Texture';$p.receipts[0].assetId=('d'*32);$c=Clone $resourceCounter;$c.eligibleCasterIdentityRows[0].texturePublications=@([pscustomobject]@{textureId=('d'*32);bindingIndex=3;publicationGeneration='11'});VerifyNativeResourcePublications $p @($s) @($c)} $true
# Page revision must prove actual copied page bytes, not just a reloaded mesh.
$pageStep=Clone $resourceStep;$pageStep.action='page-revision';$pageStep.id='shadow-invalidation-11'
$pageEnvelope=Clone $resourceEnvelope;$pageEnvelope.receipts[0].action=$pageStep.action;$pageEnvelope.receipts[0].fixtureStepId=$pageStep.id
$pageCounter=Clone $resourceCounter;$pageCounter.fixtureStepId=$pageStep.id
foreach($field in @('pageContentRevision','payloadContentRevision')){$pageCounter.eligibleCasterIdentityRows[0]|Add-Member $field '12' -Force}
$pageCounter.eligibleCasterIdentityRows[0]|Add-Member residencyGeneration '4' -Force
$pageCounter.eligibleCasterIdentityRows[0].payloadByteSize='2048';$pageCounter.eligibleCasterIdentityRows[0].meshletCount='8';$pageCounter.eligibleCasterIdentityRows[0].payloadSource=256
Case 'page source revision reaches actual native copied page owner' {$null=VerifyNativeResourcePublications $pageEnvelope @($pageStep) @($pageCounter)}
Case 'mesh reload cannot replace missing actual page publication' {$p=Clone $pageCounter;$p.eligibleCasterIdentityRows[0].PSObject.Properties.Remove('pageContentRevision');VerifyNativeResourcePublications $pageEnvelope @($pageStep) @($p)} $true 'UNVERIFIED:'
Case 'old page payload bytes cannot pass fresh mesh revision' {$p=Clone $pageCounter;$p.eligibleCasterIdentityRows[0].payloadContentRevision='11';VerifyNativeResourcePublications $pageEnvelope @($pageStep) @($p)} $true
Case 'empty native page payload cannot prove page revision' {$p=Clone $pageCounter;$p.eligibleCasterIdentityRows[0].payloadByteSize='0';VerifyNativeResourcePublications $pageEnvelope @($pageStep) @($p)} $true
Case 'ordinary owner route cannot impersonate native copied pages' {$p=Clone $pageCounter;$p.eligibleCasterIdentityRows[0].payloadSource=0;VerifyNativeResourcePublications $pageEnvelope @($pageStep) @($p)} $true
$shaderStep=[pscustomobject]@{id='shadow-invalidation-08';action='shader-revision'}
$shaderReceipt=[pscustomobject]@{kind='ShaderPublication';fixtureStepId=$shaderStep.id;action=$shaderStep.action;shaderId=('a'*32);shaderStageId=('b'*32);shaderCollectionGeneration=20;previousShaderCollectionGeneration=19;shaderStagePublicationGeneration=12;previousObservedShaderStagePublicationGeneration=11;sourcePath=('renderer-efficiency-shaders/'+$envelope.operationId+'/Default.mesh.tspf');sourceSha256=('f'*64);actualNativeOwnerAccepted=$true;stages=@([pscustomobject]@{stage=6;shaderId=('b'*32);publicationGeneration=12;acceptedOverrideOwner=$true})}
$shaderEnvelope=[pscustomobject]@{operationId=$envelope.operationId;receipts=@($shaderReceipt)}
$shaderCounter=Clone $originalCounters;$shaderCounter.fixtureStepId=$shaderStep.id;$shaderCounter.eligibleCasterIdentityRows[0].shaderId=('a'*32);$shaderCounter.eligibleCasterIdentityRows[0]|Add-Member shaderRevision '20' -Force
Case 'shader receipt proves actual collection and accepted mesh lease' {$null=VerifyNativeShaderPublications $shaderEnvelope @($shaderStep) @($shaderCounter)}
Case 'missing actual shader receipt stays unverified' {$p=Clone $shaderEnvelope;$p.receipts=@();VerifyNativeShaderPublications $p @($shaderStep) @($shaderCounter)} $true 'UNVERIFIED:'
Case 'old captured shader collection cannot claim new lease' {$p=Clone $shaderCounter;$p.eligibleCasterIdentityRows[0].shaderRevision='19';VerifyNativeShaderPublications $shaderEnvelope @($shaderStep) @($p)} $true
Case 'unaccepted native mesh stage cannot prove shader publication' {$p=Clone $shaderEnvelope;$p.receipts[0].stages[0].acceptedOverrideOwner=$false;VerifyNativeShaderPublications $p @($shaderStep) @($shaderCounter)} $true
Case 'different mesh stage identity cannot impersonate exact lease' {$p=Clone $shaderEnvelope;$p.receipts[0].stages[0].shaderId=('c'*32);VerifyNativeShaderPublications $p @($shaderStep) @($shaderCounter)} $true
Case 'shader stage owner must advance beyond captured prior' {$p=Clone $shaderEnvelope;$p.receipts[0].shaderStagePublicationGeneration=11;VerifyNativeShaderPublications $p @($shaderStep) @($shaderCounter)} $true
Case 'shader path must belong to current actual operation' {$p=Clone $shaderEnvelope;$p.receipts[0].sourcePath='renderer-efficiency-shaders/elsewhere/Default.mesh.tspf';VerifyNativeShaderPublications $p @($shaderStep) @($shaderCounter)} $true
Case 'shader semantic bytes allow distinct owned operation paths' {$p=Clone $shaderEnvelope;$p.operationId=('c'*32);$p.receipts[0].sourcePath='renderer-efficiency-shaders/'+$p.operationId+'/Default.mesh.tspf';CompareNativeShaderPublications $p $shaderEnvelope @($shaderStep) @($shaderCounter) @($shaderCounter)}
Case 'different copied shader source bytes cannot compare equal' {$p=Clone $shaderEnvelope;$p.receipts[0].sourceSha256=('e'*64);CompareNativeShaderPublications $p $shaderEnvelope @($shaderStep) @($shaderCounter) @($shaderCounter)} $true
Case 'unused fallback shader collection receipt does not fabricate required proof' {$p=Clone $shaderEnvelope;$unused=Clone $p.receipts[0];$unused.shaderId=('d'*32);$p.receipts+=@($unused);$null=VerifyNativeShaderPublications $p @($shaderStep) @($shaderCounter)}
# Generate explicitly unavailable test data from the frozen schema. Its empty
# frames prove only parser/ownership behavior, never rendering acceptance.
$script:definitionRoot=Get-Content (Join-Path (Split-Path $Comparer) 'Schemas/evidence.schema.json') -Raw|ConvertFrom-Json
function IndexFixtureValue($rule) {
	if(Has $rule '$ref'){return IndexFixtureValue $script:definitionRoot.'$defs'.($rule.'$ref'.Split('/')[-1])}
	if(Has $rule 'const'){return $rule.const}
	if(Has $rule 'enum'){foreach($choice in @('Unavailable','Unverified')){if($choice -in $rule.enum){return $choice}};return $rule.enum[0]}
	if(Has $rule 'anyOf'){if(@($rule.anyOf|Where-Object {(Has $_ type) -and $_.type -eq 'null'}).Count){return $null};return IndexFixtureValue $rule.anyOf[0]}
	if(!(Has $rule type) -and (Has $rule allOf)){return IndexFixtureValue $rule.allOf[0]}
	if($rule.type -eq 'object'){$v=[ordered]@{};foreach($property in $rule.properties.PSObject.Properties){$v[$property.Name]=IndexFixtureValue $property.Value};if($v.Contains('reason')){$v.reason='NotAvailable'};return $v}
	if($rule.type -eq 'array'){return ,@()};if($rule.type -in @('number','integer')){if(Has $rule minimum){return $rule.minimum};return 0};if($rule.type -eq 'boolean'){return $false};if($rule.type -eq 'string'){return 'SyntheticUnavailable'};return $null
}
$indexDocument=Clone (IndexFixtureValue $script:definitionRoot);$indexDocument.identity.mode='Candidate';$indexDocument.identity.stage='P1';$indexDocument.identity.fixtureId='shadow-invalidation';$indexDocument.identity.seed=65001;$indexDocument.identity.runIndex=0
$indexPath=Join-Path $root 'index-evidence.json';[IO.File]::WriteAllText($indexPath,(Canonical $indexDocument),$utf8)
Case 'streaming index retains identity options only then fully validates' {$i=LoadEvidence @($indexPath) Candidate;NativeRequire (!(Has $i[0].value frames) -and @($i[0].value.PSObject.Properties).Count -eq 2) 'Index retained full frame document';$full=LoadIndexedEvidence $i[0];NativeRequire ($full.value.frames.Count -eq 0) 'Full document missing actual frames'}
Case 'duplicate root JSON property rejected by streaming reader' {$p=Join-Path $root 'index-duplicate.json';[IO.File]::WriteAllText($p,((Canonical $indexDocument).Insert(1,'"schemaVersion":1,')),$utf8);$null=[TerraRendererEvidenceIndex]::Read($p)} $true 'Exception calling'
Case 'duplicate skipped frame property rejected without retaining frames' {$p=Join-Path $root 'index-frame-duplicate.json';$json=(Canonical $indexDocument).Replace('"frames":[]','"frames":[{"x":1,"x":2}]');[IO.File]::WriteAllText($p,$json,$utf8);$null=[TerraRendererEvidenceIndex]::Read($p)} $true 'Exception calling'
Case 'malformed skipped frame JSON cannot hide behind index' {$p=Join-Path $root 'index-frame-malformed.json';$json=(Canonical $indexDocument).Replace('"frames":[]','"frames":[{"x":}]');[IO.File]::WriteAllText($p,$json,$utf8);$null=[TerraRendererEvidenceIndex]::Read($p)} $true 'Exception calling'
Case 'index full file mutation rejected before full parsing' {$p=Join-Path $root 'index-change.json';[IO.File]::WriteAllText($p,(Canonical $indexDocument),$utf8);$i=LoadEvidence @($p) Candidate;[IO.File]::AppendAllText($p,' ');$null=LoadIndexedEvidence $i[0]} $true 'Evidence'
Case 'forged index identity cannot bypass full document equality' {$i=LoadEvidence @($indexPath) Candidate;$i[0].value.identity.runIndex=2;$null=LoadIndexedEvidence $i[0]} $true 'immutable index'
Case 'forged index options cannot bypass full document equality' {$i=LoadEvidence @($indexPath) Candidate;$i[0].value.requestedOptions.validationPreparation='Serial';$null=LoadIndexedEvidence $i[0]} $true 'immutable index'
Case 'full schema still rejects malformed frame structure after indexing' {$p=Join-Path $root 'index-full-invalid.json';$json=(Canonical $indexDocument).Replace('"frames":[]','"frames":"not-an-array"');[IO.File]::WriteAllText($p,$json,$utf8);$i=LoadEvidence @($p) Candidate;$null=LoadIndexedEvidence $i[0]} $true 'wrong JSON type'
Case 'unrequested extra fixture input is explicitly rejected' {$i=LoadEvidence @($indexPath) Candidate;ValidateEvidenceIndexSelection $i @([pscustomobject]@{id='multiview-lights'}) P1} $true 'Unexpected evidence'
Case 'duplicate input path cannot inflate matched run counts' {$null=LoadEvidence @($indexPath,$indexPath) Candidate} $true 'Duplicate evidence input'
Case 'trailing additional JSON document rejected while indexing' {$p=Join-Path $root 'index-trailing.json';[IO.File]::WriteAllText($p,((Canonical $indexDocument)+' {}'),$utf8);$null=[TerraRendererEvidenceIndex]::Read($p)} $true 'Exception calling'
Case 'many skipped frame objects retain only bounded routing fields' {
	$p=Join-Path $root 'index-many-frames.json';$pieces=(Canonical $indexDocument).Split([string[]]@('"frames":[]'),[StringSplitOptions]::None)
	$writer=[IO.StreamWriter]::new($p,$false,$utf8);try{$writer.Write($pieces[0]+'"frames":[');foreach($i in 0..19999){if($i){$writer.Write(',')};$writer.Write('{"unvalidated":"brackets ] } inside text","metric":0}')};$writer.Write(']'+$pieces[1])}finally{$writer.Dispose()}
	$parts=[TerraRendererEvidenceIndex]::Read($p);NativeRequire ($parts.Count -eq 2 -and $parts[0].Length -lt 65536 -and $parts[1].Length -lt 65536) 'Streaming index retained skipped frame payload'
}
# The old policy verified warmup bytes in isolation and ignored first-control
# receipts. These CPU regressions exercise the exact production policy seams.
Case 'warmup depth mismatch cannot hide behind valid measured frames' {
	$a=SaveProof (Clone $originalFrame) (Clone $originalCounters)
	$d=[byte[]](@([BitConverter]::GetBytes([single]0.50002))+@([BitConverter]::GetBytes([single]1)))
	$b=SaveProof (Clone $originalFrame) (Clone $originalCounters) $d
	CompareNativeObservationProof $a $b $identity.resolution Warmup
} $true
Case 'missing first control transition cannot be replaced by measured reuse' {
	$p=Clone $envelope;$p.receipts=@()
	VerifyNativeControlTransitions $p ([pscustomobject]@{frames=@();artifacts=@();requestedOptions=$originalFrame.effectiveOptions}) $identity $root @([pscustomobject]@{id=$originalFrame.fixtureStepId;action='selected-lod'}) @([pscustomobject]@{viewType='Scene';viewId=1})
} $true 'UNVERIFIED:'
# Build the same four-light/nine-region cohort as the native P1 fixture, using
# small CPU files. Each row owns a separate hashed sidecar; this is not GPU proof.
$script:transitionSequence=0
function TransitionFixture([string]$Generation='8',[string]$RawGeneration='8',[string]$Epoch='2',[bool]$Reuse=$false) {
	$script:transitionSequence++;$prefix="transition-$script:transitionSequence"
	$frames=[Collections.Generic.List[object]]::new();$baselineFrames=[Collections.Generic.List[object]]::new();$allArtifacts=[Collections.Generic.List[object]]::new()
	$options=Clone (IndexFixtureValue $script:definitionRoot.'$defs'.options)
	$options.shadowReuse='Enabled'
	foreach($kind in 0..3){$faces=if($kind -eq 1){0..5}else{@(0)};foreach($face in $faces){
		$f=Clone (IndexFixtureValue $script:definitionRoot.'$defs'.transitionFrame)
		foreach($property in $originalFrame.PSObject.Properties){$f.($property.Name)=Clone $property.Value}
		$f.measurementPhase='Transition';$f.effectiveOptions=Clone $options
		$c=Clone $originalCounters;$c.measurementPhase='Transition';$c.effectiveOptions=Clone $options
		foreach($row in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$row.objectId='650'+('0'*27)+'11';$row.selectedLod=1;$row.firstMeshlet='22';$row.meshletCount='16';$row.segmentId='94489280528'}
		$c.readbackProvenance|Add-Member fixtureControlGeneration $RawGeneration
		$c.readbackProvenance|Add-Member optionsSessionGeneration $Epoch
		$c.qualifiedShadowViewKey.resourceKind=$kind;$c.qualifiedShadowViewKey.faceIndex=$face
		$c.qualifiedShadowViewKey.lightId=([string]($kind+5))*32
		$c.sourceContentOwner.qualifiedShadowViewKey=Clone $c.qualifiedShadowViewKey
		$f.faceIndex=Wrap $face;$f.lightId=Wrap $c.qualifiedShadowViewKey.lightId
		if($Reuse){$c.submittedCasterIdentityRows=@();$c.nativeCounters.submittedCasterSegments.value=0;$c.nativeCounters.actualDrawSubmissions.value=0;$c.nativeCounters.shadowReuses.value=1}
		$f.shadow=Clone (IndexFixtureValue $script:definitionRoot.'$defs'.shadow)
		foreach($name in @('eligibleCasterSegments','submittedCasterSegments','actualDrawSubmissions')){$f.shadow.$name=Clone $c.nativeCounters.$name}
		$f.shadow.reuseRegionCount=Clone $c.nativeCounters.shadowReuses
		$null=SaveProof $f $c
		$directory=Join-Path $root "$prefix/$kind-$face";[IO.Directory]::CreateDirectory($directory)|Out-Null
		foreach($artifact in $f.artifacts){$destination=Join-Path $directory $artifact.path;[IO.File]::Copy((Join-Path $root $artifact.path),$destination);$artifact.path="$prefix/$kind-$face/$($artifact.path)";$allArtifacts.Add($artifact)}
		$frames.Add($f)
		$b=Clone $c;$b.measurementPhase='Warmup';$b.fixtureStepId='unit-stable'
		foreach($row in @($b.eligibleCasterIdentityRows)+@($b.submittedCasterIdentityRows)){$row.selectedLod=0;$row.firstMeshlet='0';$row.meshletCount='22';$row.segmentId='22'}
		$bf=Clone $f;$bf.measurementPhase='Warmup';$bf.fixtureStepId='unit-stable'
		$null=SaveProof $bf $b
		$directory=Join-Path $root "$prefix/baseline-$kind-$face";[IO.Directory]::CreateDirectory($directory)|Out-Null
		foreach($artifact in $bf.artifacts){[IO.File]::Copy((Join-Path $root $artifact.path),(Join-Path $directory $artifact.path));$artifact.path="$prefix/baseline-$kind-$face/$($artifact.path)";$allArtifacts.Add($artifact)}
		$baselineFrames.Add($bf)
		# Admission queues a fresh correlation only after this retained transition.
		# Keep actual hashed warmup bytes, with a later native frame and its own
		# mutation clock; never synthesize an ordering relation between epochs.
		$w=Clone $c;$w.measurementPhase='Warmup';$wf=Clone $f;$wf.measurementPhase='Warmup'
		foreach($pair in @(@('frameSerial',8),@('commandSerial',9),@('submitSerial',10),@('completionSerial',10),@('mutationGeneration',11))){$w.readbackProvenance.($pair[0])=[string]$pair[1];$wf.($pair[0])=Wrap $pair[1]}
		$w.readbackProvenance.fixtureControlGeneration='11';$null=SaveProof $wf $w
		$directory=Join-Path $root "$prefix/warmup-$kind-$face";[IO.Directory]::CreateDirectory($directory)|Out-Null
		foreach($artifact in $wf.artifacts){[IO.File]::Copy((Join-Path $root $artifact.path),(Join-Path $directory $artifact.path));$artifact.path="$prefix/warmup-$kind-$face/$($artifact.path)";$allArtifacts.Add($artifact)}
		$baselineFrames.Add($wf)
	}}
	$receipt=[pscustomobject]@{kind='ControlTransition';fixtureStepId=$originalFrame.fixtureStepId;action='selected-lod';controlGeneration=$Generation;originalCorrelation=[pscustomobject]@{operationId=$envelope.operationId;fixtureStepId=$originalFrame.fixtureStepId;stepFrame=0;mutationGeneration=3;settingsGeneration=2;rendererSessionGeneration=4};frameSerial='5';commandSerial='6';submitSerial='7';completionSerial='7';frames=$frames.ToArray();artifacts=@($frames.ToArray()|ForEach-Object artifacts)}
	$p=Clone $envelope;$p.receipts=@($receipt)
	return @{envelope=$p;document=[pscustomobject]@{frames=$baselineFrames.ToArray();artifacts=$allArtifacts.ToArray();requestedOptions=$options};steps=@([pscustomobject]@{id='unit-stable';action='stable'},[pscustomobject]@{id=$originalFrame.fixtureStepId;action='selected-lod'});cameras=@([pscustomobject]@{viewType='Scene';viewId=1})}
}
function VerifyTransitionFixture($fixture){return VerifyNativeControlTransitions $fixture.envelope $fixture.document $identity $root $fixture.steps $fixture.cameras}
function ChangeTransitionCommand($fixture,[int]$Index,[int]$Command) {
	$f=$fixture.envelope.receipts[0].frames[$Index];$f.commandSerial=Wrap $Command
	$artifact=@($f.artifacts|Where-Object kind -eq Counters)[0];$path=Join-Path $root $artifact.path
	$c=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json;$c.readbackProvenance.commandSerial=[string]$Command
	[IO.File]::WriteAllText($path,(Canonical $c),$utf8);$artifact.sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
}
function ChangeTransitionCounters($fixture,[scriptblock]$Change) {
	foreach($f in $fixture.envelope.receipts[0].frames){$artifact=@($f.artifacts|Where-Object kind -eq Counters)[0];$path=Join-Path $root $artifact.path
		$c=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json;& $Change $c
		[IO.File]::WriteAllText($path,(Canonical $c),$utf8);$artifact.sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
	}
}
function AddTransitionGameView($fixture) {
	$r=$fixture.envelope.receipts[0];$game=@()
	foreach($original in @($r.frames)+@($fixture.document.frames)){
		$f=Clone $original;$f.viewType='Game';$f.viewId=0;$f.commandSerial=Wrap 10
		foreach($artifact in $f.artifacts){$source=Join-Path $root $artifact.path;$artifact.path="game/$($artifact.path)";$destination=Join-Path $root $artifact.path;[IO.Directory]::CreateDirectory((Split-Path $destination))|Out-Null;[IO.File]::Copy($source,$destination)
			if($artifact.kind -eq 'Counters'){$c=Get-Content -LiteralPath $destination -Raw|ConvertFrom-Json;$c.readbackProvenance.commandSerial='10';$c.qualifiedShadowViewKey.viewType='Game';$c.qualifiedShadowViewKey.viewId=0;$c.sourceContentOwner.qualifiedShadowViewKey=Clone $c.qualifiedShadowViewKey;[IO.File]::WriteAllText($destination,(Canonical $c),$utf8);$artifact.sha256=(Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant()}
		}
		if($f.measurementPhase -eq 'Transition'){$game+=@($f)}else{$fixture.document.frames+=@($f);$fixture.document.artifacts+=@($f.artifacts)}
	}
	$r.frames=@($r.frames)+$game;$r.artifacts=@($r.artifacts)+@($game|ForEach-Object artifacts)
	$fixture.document.artifacts=@($fixture.document.artifacts)+@($game|ForEach-Object artifacts);$fixture.cameras+=@([pscustomobject]@{viewType='Game';viewId=0})
}
Case 'complete first nine-region transition uses exact production schema and bytes' {$f=TransitionFixture;$p=VerifyTransitionFixture $f;$null=CompareNativeControlTransitions $p $p $identity.resolution}
Case 'complete eighteen-region transition preserves distinct Scene and Game commands' {$f=TransitionFixture;AddTransitionGameView $f;$p=VerifyTransitionFixture $f;$null=CompareNativeControlTransitions $p $p $identity.resolution}
Case 'same-view transition cannot mix actual command serials' {$f=TransitionFixture;ChangeTransitionCommand $f 2 10;VerifyTransitionFixture $f} $true
Case 'first transition missing cube face rejected' {$f=TransitionFixture;$f.envelope.receipts[0].frames=@($f.envelope.receipts[0].frames|Where-Object {!(($_.faceIndex.value -eq 5))});VerifyTransitionFixture $f} $true
Case 'first transition missing entire light rejected' {$f=TransitionFixture;$f.envelope.receipts[0].frames=@($f.envelope.receipts[0].frames|Where-Object {$_.lightId.value -cne ('8'*32)});VerifyTransitionFixture $f} $true
Case 'first transition raw generation must equal admitted control' {VerifyTransitionFixture (TransitionFixture '9' '8')} $true
Case 'first transition zero native options epoch rejected' {VerifyTransitionFixture (TransitionFixture '8' '8' '0')} $true
Case 'first transition original mutation cannot be replaced by later admission' {$f=TransitionFixture;$f.envelope.receipts[0].originalCorrelation.mutationGeneration=99;VerifyTransitionFixture $f} $true
Case 'first transition stale original operation rejected' {$f=TransitionFixture;$f.envelope.receipts[0].originalCorrelation.operationId='9'*32;VerifyTransitionFixture $f} $true
Case 'first transition changed selected range cannot claim old hit' {VerifyTransitionFixture (TransitionFixture '8' '8' '2' $true)} $true
Case 'declared LOD control must change actual target from hashed stable baseline' {$f=TransitionFixture;ChangeTransitionCounters $f {param($c)foreach($r in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$r.selectedLod=0;$r.firstMeshlet='0';$r.meshletCount='22';$r.segmentId='22'}};VerifyTransitionFixture $f} $true
Case 'meshlet transition matches actual exclude-first controller range' {$f=TransitionFixture;$f.steps[1].action='selected-meshlet';$f.envelope.receipts[0].action='selected-meshlet';ChangeTransitionCounters $f {param($c)foreach($r in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$r.selectedLod=0;$r.firstMeshlet='1';$r.meshletCount='21';$r.segmentId='4294967317'}};$null=VerifyTransitionFixture $f}
Case 'unchanged meshlet span cannot impersonate declared control' {$f=TransitionFixture;$f.steps[1].action='selected-meshlet';$f.envelope.receipts[0].action='selected-meshlet';ChangeTransitionCounters $f {param($c)foreach($r in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$r.selectedLod=0;$r.firstMeshlet='0';$r.meshletCount='22';$r.segmentId='22'}};VerifyTransitionFixture $f} $true
Case 'arbitrary smaller span cannot impersonate frozen exclude-first control' {$f=TransitionFixture;$f.steps[1].action='selected-meshlet';$f.envelope.receipts[0].action='selected-meshlet';ChangeTransitionCounters $f {param($c)foreach($r in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$r.selectedLod=0;$r.firstMeshlet='1';$r.meshletCount='1';$r.segmentId='4294967297'}};VerifyTransitionFixture $f} $true
Case 'selection baseline missing remains unverified' {$f=TransitionFixture;$f.document.frames=@($f.document.frames|Where-Object fixtureStepId -CNE unit-stable);VerifyTransitionFixture $f} $true 'UNVERIFIED:'
Case 'ack-only transition without completed native rows rejected by schema' {$f=TransitionFixture;$f.envelope.receipts[0].frames=@();VerifyTransitionFixture $f} $true 'too few entries'
Case 'transition zero control generation rejected by schema' {VerifyTransitionFixture (TransitionFixture '0')} $true 'invalid pattern'
Case 'transition prepared row cannot impersonate completed write' {$f=TransitionFixture;$f.envelope.receipts[0].frames[0].publicationState='Prepared';VerifyTransitionFixture $f} $true
Case 'later completed measurement cannot be relabeled first transition' {$f=TransitionFixture;$f.envelope.receipts[0].frameSerial='12';foreach($row in $f.envelope.receipts[0].frames){$row.frameSerial=Wrap 12};ChangeTransitionCounters $f {param($c)$c.readbackProvenance.frameSerial='12'};VerifyTransitionFixture $f} $true
Case 'old mutation and admitted control clocks need no numeric ordering' {$f=TransitionFixture;$f.envelope.receipts[0].originalCorrelation.mutationGeneration=436;foreach($row in $f.envelope.receipts[0].frames){$row.mutationGeneration=Wrap 436};ChangeTransitionCounters $f {param($c)$c.readbackProvenance.mutationGeneration='436'};$null=VerifyTransitionFixture $f}
Case 'frozen absolute phase boundaries remain valid' {foreach($pair in @(@('Warmup',0),@('Warmup',59),@('Measured',60),@('Measured',359))){VerifyNativeObservationPhase ([pscustomobject]@{measurementPhase=$pair[0];stepFrame=$pair[1]})}}
Case 'warmup cannot move into measured absolute window' {VerifyNativeObservationPhase ([pscustomobject]@{measurementPhase='Warmup';stepFrame=60})} $true
Case 'measured cannot hide an early warmup observation' {VerifyNativeObservationPhase ([pscustomobject]@{measurementPhase='Measured';stepFrame=59})} $true
Case 'extra measured frame cannot extend frozen sample window' {VerifyNativeObservationPhase ([pscustomobject]@{measurementPhase='Measured';stepFrame=360})} $true
Case 'transition pair must compare actual qualified origin even with identical pixels' {
	$a=TransitionFixture;$left=VerifyTransitionFixture $a;$b=TransitionFixture
	ChangeTransitionCounters $b {param($c)$c.qualifiedShadowViewKey.referenceOrigin.cell[0]='1';$c.sourceContentOwner.qualifiedShadowViewKey=Clone $c.qualifiedShadowViewKey}
	$right=VerifyTransitionFixture $b;CompareNativeControlTransitions $left $right $identity.resolution
} $true
Case 'warmup semantic mismatch cannot hide behind identical pixels' {
	$a=SaveProof (Clone $originalFrame) (Clone $originalCounters);$c=Clone $originalCounters
	$c.eligibleCasterIdentityRows[0].selectedLod=1;$c.submittedCasterIdentityRows[0].selectedLod=1
	$b=SaveProof (Clone $originalFrame) $c;CompareNativeObservationProof $a $b $identity.resolution Warmup
} $true
Case 'unchanged origin control may truthfully reuse complete content' {$f=TransitionFixture '8' '8' '2' $true;$f.steps[1].action='origin';$f.envelope.receipts[0].action='origin';$null=VerifyTransitionFixture $f}
Case 'transition duplicate receipt rejected' {$f=TransitionFixture;$f.envelope.receipts=@($f.envelope.receipts[0],$f.envelope.receipts[0]);VerifyTransitionFixture $f} $true 'UNVERIFIED:'
Case 'transition artifact must remain in root closure' {$f=TransitionFixture;$f.document.artifacts=@($f.document.artifacts|Select-Object -Skip 1);VerifyTransitionFixture $f} $true
Case 'complete root descriptor retains ordinal path case' {$f=TransitionFixture;$f.document.artifacts=Clone $f.document.artifacts;$f.document.artifacts[0].path=$f.document.artifacts[0].path.ToUpperInvariant();VerifyTransitionFixture $f} $true
Case 'same path and hash cannot hide different root descriptor metadata' {$f=TransitionFixture;$f.document.artifacts=Clone $f.document.artifacts;$f.document.artifacts[0].reason='Different descriptor';VerifyTransitionFixture $f} $true
Case 'transition raw artifact tampering rejected' {$f=TransitionFixture;[IO.File]::AppendAllText((Join-Path $root $f.envelope.receipts[0].artifacts[0].path),'x');VerifyTransitionFixture $f} $true 'Artifact'
Case 'warmup image mismatch cannot hide behind valid measured frames' {
	$a=SaveProof (Clone $originalFrame) (Clone $originalCounters)
	$bitmap=[Drawing.Bitmap]::new(7,5);$stream=[IO.MemoryStream]::new()
	try{$bitmap.SetPixel(0,0,[Drawing.Color]::FromArgb(255,200,20,10));$bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png);$bad=$stream.ToArray()}finally{$bitmap.Dispose();$stream.Dispose()}
	$b=SaveProof (Clone $originalFrame) (Clone $originalCounters) $depth $coverage $bad
	CompareNativeObservationProof $a $b $identity.resolution Warmup
} $true
function ResidencyFixture([bool]$Reuse=$false,[bool]$Game=$false) {
	# Two genuinely distinct owned CPU artifact cohorts model native serialization,
	# not a GPU run. The actual host baseline token is the frozen 60+300 boundary.
	$a=TransitionFixture;$b=TransitionFixture '8' '8' '2' $Reuse
	if($Game){AddTransitionGameView $a;AddTransitionGameView $b}
	foreach($entry in @(@($a,360,5,1),@($b,361,6,2))) {
		$x=$entry[0];$r=$x.envelope.receipts[0];$r.originalCorrelation.stepFrame=$entry[1];$r.frameSerial=[string]$entry[2]
		foreach($f in $r.frames){$f.stepFrame=$entry[1];$f.frameSerial=Wrap $entry[2]}
		$sample=$entry[1];$serial=$entry[2];$allocation=$entry[3]
		ChangeTransitionCounters $x {param($c)$c.stepFrame=[string]$sample;$c.readbackProvenance.frameSerial=[string]$serial;foreach($row in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$row|Add-Member meshRevision '14' -Force;$row|Add-Member residencyGeneration ([string]$allocation) -Force}}
	}
	$base=$a.envelope.receipts[0];$new=$b.envelope.receipts[0]
	function Observation($r){$o=[ordered]@{};foreach($field in @('controlGeneration','originalCorrelation','frameSerial','commandSerial','submitSerial','completionSerial','frames','artifacts')){$o[$field]=$r.$field};return [pscustomobject]$o}
	$r=[pscustomobject]@{kind='ResidencyPublication';fixtureStepId=$base.fixtureStepId;targetObjectId=('650'+('0'*27)+'11');meshId=('3'*32);previousMeshPublicationGeneration=14;newMeshPublicationGeneration=14;previousAllocationGeneration=1;newAllocationGeneration=2;previousActualFrameSerial=5;newActualFrameSerial=6;commandSerial=6;submitSerial=7;completionSerial=7;baselineObservation=(Observation $base);reuploadObservation=(Observation $new)}
	$a.envelope.receipts=@($r);$a.document.artifacts+=@($new.artifacts)
	$a.steps=@([pscustomobject]@{id=$base.fixtureStepId;action='residency';warmupFrames=60;measuredFrames=300})
	return $a
}
function VerifyResidencyFixture($f) {
	# For meaningful old-policy red, invoke its real existing guard: it ignores
	# residency and accepts these absent/unproven receipts. No missing-symbol red.
	if(Get-Command VerifyNativeResidencyPublications -ErrorAction SilentlyContinue){return VerifyNativeResidencyPublications $f.envelope $f.document $identity $root $f.steps $f.cameras}
	return VerifyNativeControlTransitions $f.envelope $f.document $identity $root $f.steps $f.cameras
}
Case 'residency missing receipt cannot pass later warmup parity' {$f=ResidencyFixture;$f.envelope.receipts=@();VerifyResidencyFixture $f} $true 'UNVERIFIED:'
Case 'residency scalar-only receipt cannot pass without original native cohorts' {$f=ResidencyFixture;$f.envelope.receipts[0].PSObject.Properties.Remove('baselineObservation');$f.envelope.receipts[0].PSObject.Properties.Remove('reuploadObservation');VerifyResidencyFixture $f} $true 'missing baselineObservation'
Case 'residency reused reupload cannot stand in for first refresh' {VerifyResidencyFixture (ResidencyFixture $true)} $true
Case 'real manifest shape without target validates complete nine-region residency' {$f=ResidencyFixture;Require (!(Has $f.steps[0] target)) 'Synthetic step differs from real manifest';$p=VerifyResidencyFixture $f;$null=CompareNativeControlTransitions $p $p $identity.resolution}
Case 'residency receipt cannot change frozen installed grid target' {$f=ResidencyFixture;$f.envelope.receipts[0].targetObjectId='8'*32;VerifyResidencyFixture $f} $true
Case 'unsupported residency fixture cannot inherit P1 grid contract' {$f=ResidencyFixture;$other=Clone $identity;$other.fixtureId='other-fixture';VerifyNativeResidencyPublications $f.envelope $f.document $other $root $f.steps $f.cameras} $true 'UNVERIFIED:'
Case 'complete eighteen-region residency allows scalar second-view command' {$f=ResidencyFixture $false $true;$f.envelope.receipts[0].commandSerial=10;$p=VerifyResidencyFixture $f;$null=CompareNativeControlTransitions $p $p $identity.resolution}
Case 'residency invented scalar command cannot name another view' {$f=ResidencyFixture $false $true;$f.envelope.receipts[0].commandSerial=99;VerifyResidencyFixture $f} $true
Case 'residency missing cube face fails closed' {$f=ResidencyFixture;$r=$f.envelope.receipts[0];$r.reuploadObservation.frames=@($r.reuploadObservation.frames|Select-Object -Skip 1);VerifyResidencyFixture $f} $true
Case 'residency no allocation change rejected' {$f=ResidencyFixture;$f.envelope.receipts[0].newAllocationGeneration=1;VerifyResidencyFixture $f} $true
Case 'residency scalar publication does not override native owner' {$f=ResidencyFixture;$f.envelope.receipts[0].previousMeshPublicationGeneration=15;$f.envelope.receipts[0].newMeshPublicationGeneration=15;VerifyResidencyFixture $f} $true
Case 'residency scalar serial does not override native completion' {$f=ResidencyFixture;$f.envelope.receipts[0].completionSerial=8;VerifyResidencyFixture $f} $true
Case 'residency delayed cohort after warmup cannot be first observation' {$f=ResidencyFixture;$f.envelope.receipts[0].reuploadObservation.frameSerial='9';$f.envelope.receipts[0].newActualFrameSerial=9;VerifyResidencyFixture $f} $true
Case 'residency wrong original sample token rejected' {$f=ResidencyFixture;$f.envelope.receipts[0].baselineObservation.originalCorrelation.stepFrame=0;VerifyResidencyFixture $f} $true
Case 'residency wrong native generation cannot acknowledge new cohort' {$f=ResidencyFixture;$f.envelope.receipts[0].reuploadObservation.controlGeneration='9';VerifyResidencyFixture $f} $true
Case 'residency hash root closure remains mandatory' {$f=ResidencyFixture;$f.document.artifacts=@($f.document.artifacts|Select-Object -Skip 1);VerifyResidencyFixture $f} $true
function ChangeResidencyCounters($f,[string]$Phase,[scriptblock]$Change){$o=$f.envelope.receipts[0].($Phase+'Observation');ChangeTransitionCounters @{envelope=[pscustomobject]@{receipts=@($o)}} $Change}
Case 'residency original native options epoch cannot change' {$f=ResidencyFixture;ChangeResidencyCounters $f reupload {param($c)$c.readbackProvenance.optionsSessionGeneration='3'};VerifyResidencyFixture $f} $true
Case 'residency matching scalar cannot conceal different mesh owner' {$f=ResidencyFixture;ChangeResidencyCounters $f reupload {param($c)foreach($r in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$r.assetId='9'*32}};VerifyResidencyFixture $f} $true
Case 'residency cannot conceal unrelated local resource revision' {$f=ResidencyFixture;ChangeResidencyCounters $f reupload {param($c)foreach($r in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$r|Add-Member materialRevision '99'}};VerifyResidencyFixture $f} $true
Case 'residency same process raw generation cannot differ' {$f=ResidencyFixture;ChangeResidencyCounters $f reupload {param($c)$c.readbackProvenance.fixtureControlGeneration='9'};VerifyResidencyFixture $f} $true
Case 'residency retained reupload depth participates in paired tolerance' {$a=ResidencyFixture;$left=VerifyResidencyFixture $a;$b=ResidencyFixture;$artifact=@($b.envelope.receipts[0].reuploadObservation.frames[0].artifacts|Where-Object kind -eq Depth)[0];[IO.File]::WriteAllBytes((Join-Path $root $artifact.path),[byte[]](@([BitConverter]::GetBytes([single]0.50002))+@([BitConverter]::GetBytes([single]1))));$artifact.sha256=(Get-FileHash (Join-Path $root $artifact.path)).Hash.ToLowerInvariant();$right=VerifyResidencyFixture $b;CompareNativeControlTransitions $left $right $identity.resolution} $true
Case 'independent residency publication clocks retain immutable comparison' {$a=ResidencyFixture;$left=VerifyResidencyFixture $a;$b=ResidencyFixture;$b.envelope.receipts[0].previousMeshPublicationGeneration=19;$b.envelope.receipts[0].newMeshPublicationGeneration=19;foreach($phase in @('baseline','reupload')){ChangeResidencyCounters $b $phase {param($c)foreach($r in @($c.eligibleCasterIdentityRows)+@($c.submittedCasterIdentityRows)){$r.meshRevision='19'}}};$right=VerifyResidencyFixture $b;$null=CompareNativeControlTransitions $left $right $identity.resolution}
# Focused lifetime verification fixtures are CPU rows, never GPU acceptance.
function LifetimeFixture([string]$Old='1',[string]$New='2',[bool]$Multi=$false) {
	$all=[Collections.Generic.List[object]]::new();$views=@(,@('Scene',1));if($Multi){$views+=,@('Game',2)}
	foreach($ordinal in 14..16) { foreach($view in $views) { foreach($light in @('02','41','42','43')) {
		$faces=if($light -ceq '02'){@(0..5)}else{@(0)}
		foreach($face in $faces) {
			$c=Clone $originalCounters;$c.fixtureStepId='shadow-invalidation-'+$ordinal;$c.measurementPhase='Warmup';$c.stepFrame='0'
			$c.qualifiedShadowViewKey.viewType=$view[0];$c.qualifiedShadowViewKey.viewId=$view[1];$c.qualifiedShadowViewKey.lightId='650000000000000000000000000000'+$(if($ordinal -eq 14 -and $light -ceq '02'){'40'}else{$light})
			$c.qualifiedShadowViewKey.resourceKind=$(if($light -ceq '02'){1}else{2});$c.qualifiedShadowViewKey.faceIndex=$face
			$c.readbackProvenance|Add-Member fixtureControlGeneration ([string]($ordinal+2));$c.readbackProvenance|Add-Member optionsSessionGeneration '1';$c.readbackProvenance.settingsGeneration=[string]$ordinal;$c.readbackProvenance.mutationGeneration=[string]($ordinal+1);$c.readbackProvenance.frameSerial=[string]($ordinal*10);$c.readbackProvenance.commandSerial=[string]($ordinal*10+[int]$view[1]);$c.readbackProvenance.submitSerial=[string]($ordinal*10+5);$c.readbackProvenance.completionSerial=[string]($ordinal*10+5)
			$target=Clone $originalCounters.eligibleCasterIdentityRows[0];$target.objectId='65000000000000000000000000000010';$target.lifetimeGeneration=$(if($ordinal -eq 16){$New}else{$Old})
			$c.eligibleCasterIdentityRows=@();if($ordinal -ne 15){$c.eligibleCasterIdentityRows=@($target)}
			$c.submittedCasterIdentityRows=@($c.eligibleCasterIdentityRows)
			foreach($field in @('eligibleCasterSegments','submittedCasterSegments','actualDrawSubmissions')){$c.nativeCounters.$field.value=$c.eligibleCasterIdentityRows.Count};$c.nativeCounters.shadowReuses.value=0
			$all.Add($c)
		}
	} } }
	return ,$all.ToArray()
}
$lifetimeSteps=@([pscustomobject]@{id='shadow-invalidation-14';action='light-assignment'},[pscustomobject]@{id='shadow-invalidation-15';action='membership-removal'},[pscustomobject]@{id='shadow-invalidation-16';action='lifetime-replacement'})
$lifetimeScene=@([pscustomobject]@{viewType='Scene';viewId=1})
$lifetimeTwoViews=@([pscustomobject]@{viewType='Scene';viewId=1},[pscustomobject]@{viewType='Game';viewId=2})
function VerifyLifetimeFixture($Rows,$Views=$lifetimeScene) {
	if(Get-Command VerifyNativeLifetimeReplacement -ErrorAction SilentlyContinue) { VerifyNativeLifetimeReplacement $Rows $lifetimeSteps 'shadow-invalidation' $Views;return }
	# The previous real policy validates complete bundles and cross-process caster
	# semantics, deliberately excluding numeric lifetimes; it has no action join.
	foreach($ordinal in 14..16){$cohort=@($Rows|Where-Object fixtureStepId -ceq ('shadow-invalidation-'+$ordinal));VerifyNativeBundle $cohort;foreach($c in $cohort){$null=NativeCasterRows $c.eligibleCasterIdentityRows $true}}
}
Case 'lifetime valid complete single and multi-view timeline' {VerifyLifetimeFixture (LifetimeFixture);VerifyLifetimeFixture (LifetimeFixture '1' '2' $true) $lifetimeTwoViews}
Case 'lifetime independent process clocks remain valid' {$a=LifetimeFixture '1' '2';$b=LifetimeFixture '99' '101';VerifyLifetimeFixture $a;VerifyLifetimeFixture $b;$left=@($a|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$right=@($b|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];NativeRequire (((NativeCasterRows $left.eligibleCasterIdentityRows $true)-join "`n") -ceq ((NativeCasterRows $right.eligibleCasterIdentityRows $true)-join "`n")) 'Cross-process lifetime numbers entered semantic comparison'}
Case 'lifetime unchanged replacement generation rejected' {VerifyLifetimeFixture (LifetimeFixture '1' '1')} $true
Case 'lifetime wrong replacement UUID rejected' {$f=LifetimeFixture;foreach($c in @($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')){$c.eligibleCasterIdentityRows[0].objectId='7'*32;$c.submittedCasterIdentityRows[0].objectId='7'*32};VerifyLifetimeFixture $f} $true
Case 'lifetime removed target still eligible rejected' {$f=LifetimeFixture;$old=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-14')[0].eligibleCasterIdentityRows;foreach($c in @($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-15')){$c.eligibleCasterIdentityRows=Clone $old};VerifyLifetimeFixture $f} $true
Case 'lifetime missing target region rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.eligibleCasterIdentityRows=@();$last.submittedCasterIdentityRows=@();VerifyLifetimeFixture $f} $true
Case 'lifetime duplicate target segment rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$extra=Clone $last.eligibleCasterIdentityRows[0];$extra.segmentId='1';$last.eligibleCasterIdentityRows+=@($extra);VerifyLifetimeFixture $f} $true
Case 'lifetime zero replacement generation rejected' {VerifyLifetimeFixture (LifetimeFixture '1' '0')} $true
Case 'lifetime different new generations between regions rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.eligibleCasterIdentityRows[0].lifetimeGeneration='3';$last.submittedCasterIdentityRows[0].lifetimeGeneration='3';VerifyLifetimeFixture $f} $true
Case 'lifetime old shadow reuse on first replacement rejected' {$f=LifetimeFixture;foreach($c in @($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')){$c.nativeCounters.shadowReuses.value=1;$c.nativeCounters.actualDrawSubmissions.value=0;$c.nativeCounters.submittedCasterSegments.value=0;$c.submittedCasterIdentityRows=@()};VerifyLifetimeFixture $f} $true
Case 'lifetime submitted old owner cannot claim new eligible target' {$f=LifetimeFixture;foreach($c in @($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')){$c.submittedCasterIdentityRows=Clone $c.eligibleCasterIdentityRows;$c.submittedCasterIdentityRows[0].lifetimeGeneration='1'};VerifyLifetimeFixture $f} $true
Case 'lifetime missing required cohort stays unverified' {$all=LifetimeFixture;$f=@($all|Where-Object fixtureStepId -cne 'shadow-invalidation-15');VerifyLifetimeFixture $f} $true 'UNVERIFIED:'
Case 'lifetime point assignment must retain actual step14 light40' {$f=LifetimeFixture;foreach($c in @($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-14')){if($c.qualifiedShadowViewKey.lightId -ceq '65000000000000000000000000000040'){$c.qualifiedShadowViewKey.lightId='65000000000000000000000000000002'}};VerifyLifetimeFixture $f} $true
Case 'lifetime mixed cohort settingsGeneration rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.readbackProvenance.settingsGeneration='99';VerifyLifetimeFixture $f} $true
Case 'lifetime mixed cohort mutationGeneration rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.readbackProvenance.mutationGeneration='99';VerifyLifetimeFixture $f} $true
Case 'lifetime mixed cohort rendererSessionGeneration rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.readbackProvenance.rendererSessionGeneration='99';VerifyLifetimeFixture $f} $true
Case 'lifetime mixed cohort fixtureControlGeneration rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.readbackProvenance.fixtureControlGeneration='99';VerifyLifetimeFixture $f} $true
Case 'lifetime mixed cohort optionsSessionGeneration rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.readbackProvenance.optionsSessionGeneration='99';VerifyLifetimeFixture $f} $true
Case 'lifetime zero native control generation rejected' {$f=LifetimeFixture;foreach($c in $f){$c.readbackProvenance.fixtureControlGeneration='0'};VerifyLifetimeFixture $f} $true
Case 'lifetime mixed cohort effective options rejected' {$f=LifetimeFixture;$last=@($f|Where-Object fixtureStepId -ceq 'shadow-invalidation-16')[0];$last.effectiveOptions.shadowReuse='AlwaysRefresh';VerifyLifetimeFixture $f} $true
Case 'lifetime unexpected observed qualified view rejected' {$f=LifetimeFixture;foreach($c in $f){$c.qualifiedShadowViewKey.viewType='Game';$c.qualifiedShadowViewKey.viewId=2};VerifyLifetimeFixture $f} $true
Case 'lifetime missing declared second view rejected' {VerifyLifetimeFixture (LifetimeFixture) $lifetimeTwoViews} $true
Case 'lifetime duplicate declared view rejected' {VerifyLifetimeFixture (LifetimeFixture) @($lifetimeScene[0],$lifetimeScene[0])} $true
Case 'lifetime other fixtures do not inherit frozen recipe' {if(Get-Command VerifyNativeLifetimeReplacement -ErrorAction SilentlyContinue){VerifyNativeLifetimeReplacement @() $lifetimeSteps 'multiview-lights' $lifetimeScene}}

# Artifact paths remain fresh per reference: these CPU filesystem cases do not
# claim native GPU acceptance. Every temporary path stays inside caller WorkRoot.
$artifactPathCaseRoot=Join-Path $root 'artifact-path-cases'
$artifactPathTarget=Join-Path $artifactPathCaseRoot 'target'
$artifactPathJunction=Join-Path $artifactPathCaseRoot 'junction'
$artifactPathFile=Join-Path $artifactPathTarget 'payload.bin'
foreach($ownedPath in @($artifactPathCaseRoot,$artifactPathTarget,$artifactPathJunction,$artifactPathFile)){
	Require ([IO.Path]::GetFullPath($ownedPath).StartsWith($root.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'Artifact path test fixture escapes owned WorkRoot'
}
Require (!(Test-Path -LiteralPath $artifactPathCaseRoot)) 'Artifact path test fixture must be fresh'
try{
	[IO.Directory]::CreateDirectory($artifactPathTarget)|Out-Null
	[IO.File]::WriteAllBytes($artifactPathFile,[byte[]]@(1,2,3,4))
	$artifactPathSha=(Get-FileHash -LiteralPath $artifactPathFile -Algorithm SHA256).Hash.ToLowerInvariant()
	# The link points only to our own target; deletion below removes the link
	# itself without traversing or recursively deleting that target.
	New-Item -ItemType Junction -Path $artifactPathJunction -Target $artifactPathTarget|Out-Null
	$ownedArtifactRecord=[pscustomobject]@{kind='Counters';status='Valid';path='target/payload.bin';sha256=$artifactPathSha;reason=''}
	Case 'ArtifactPathCase ordinary file retains real SHA proof' {$null=ArtifactPath $ownedArtifactRecord $artifactPathCaseRoot}
	Case 'ArtifactPathCase directory cannot replace artifact file' {$record=[pscustomobject]@{kind='Counters';status='Valid';path='target';sha256=$artifactPathSha;reason=''};$null=ArtifactPath $record $artifactPathCaseRoot} $true 'Missing artifact:'
	Case 'ArtifactPathCase nonexistent leaf remains rejected' {$record=[pscustomobject]@{kind='Counters';status='Valid';path='target/missing/payload.bin';sha256=$artifactPathSha;reason=''};$null=ArtifactPath $record $artifactPathCaseRoot} $true 'Missing artifact:'
	Case 'ArtifactPathCase empty path cannot identify artifact' {$record=[pscustomobject]@{kind='Counters';status='Valid';path='';sha256=$artifactPathSha;reason=''};$null=ArtifactPath $record $artifactPathCaseRoot} $true 'Missing artifact:'
	Case 'ArtifactPathCase absolute root cannot escape relative contract' {$record=[pscustomobject]@{kind='Counters';status='Valid';path=[IO.Path]::GetPathRoot($root);sha256=$artifactPathSha;reason=''};$null=ArtifactPath $record $artifactPathCaseRoot} $true 'Artifact path must be relative'
	Case 'ArtifactPathCase real junction file remains rejected' {$record=[pscustomobject]@{kind='Counters';status='Valid';path='junction/payload.bin';sha256=$artifactPathSha;reason=''};$null=ArtifactPath $record $artifactPathCaseRoot} $true 'Artifact cannot be redirected'
	Case 'ArtifactPathCase nonexistent leaf beneath junction remains rejected' {$record=[pscustomobject]@{kind='Counters';status='Valid';path='junction/missing/payload.bin';sha256=$artifactPathSha;reason=''};$null=ArtifactPath $record $artifactPathCaseRoot} $true 'Missing artifact:'
}finally{
	if(Test-Path -LiteralPath $artifactPathJunction){
		Require (([IO.File]::GetAttributes($artifactPathJunction) -band [IO.FileAttributes]::ReparsePoint) -ne 0) 'Expected owned test junction'
		[IO.Directory]::Delete($artifactPathJunction)
	}
	if(Test-Path -LiteralPath $artifactPathFile){Remove-Item -LiteralPath $artifactPathFile -Force}
	if(Test-Path -LiteralPath $artifactPathTarget){Remove-Item -LiteralPath $artifactPathTarget -Force}
	if(Test-Path -LiteralPath $artifactPathCaseRoot){Remove-Item -LiteralPath $artifactPathCaseRoot -Force}
}

# Focused decoded-image cases use the same Case harness and production functions.
function DecodedPng([int]$Alpha,[int]$Red,[int]$Green,[int]$Blue){
 $b=[Drawing.Bitmap]::new(7,5,[Drawing.Imaging.PixelFormat]::Format32bppArgb);$m=[IO.MemoryStream]::new()
 try{$b.SetPixel(0,0,[Drawing.Color]::FromArgb($Alpha,$Red,$Green,$Blue));$b.Save($m,[Drawing.Imaging.ImageFormat]::Png);return ,$m.ToArray()}finally{$b.Dispose();$m.Dispose()}
}
Case 'DecodedImageCase exact raw and owned channel equivalence' {
 $a=DecodedPng 255 30 20 10
 foreach($channel in @(@(255,31,20,10),@(255,30,22,10),@(255,30,20,13),@(251,30,20,10))){
  $b=DecodedPng @channel;$raw=[TerraRendererNativeProof]::ImageDelta($a,$b,7,5)
  $decoded=[TerraRendererNativeProof]::DecodedImageDelta([TerraRendererNativeProof]::DecodeImage($a,7,5),[TerraRendererNativeProof]::DecodeImage($b,7,5),7,5)
  $expected=[Math]::Max([Math]::Abs(255-$channel[0]),[Math]::Max([Math]::Abs(30-$channel[1]),[Math]::Max([Math]::Abs(20-$channel[2]),[Math]::Abs(10-$channel[3]))))/255.0
  Require ($raw -eq $decoded -and $decoded -eq $expected) 'Decoded channel error differs from exact raw reference'
 }
}
Case 'DecodedImageCase preserves alpha tolerance rejection' {
 $p=SaveProof (Clone $originalFrame) (Clone $originalCounters) $depth $coverage (DecodedPng 255 30 20 10)
 $q=SaveProof (Clone $originalFrame) (Clone $originalCounters) $depth $coverage (DecodedPng 251 30 20 10)
 $null=CompareNativeProof $p $q $identity.resolution
} $true
Case 'DecodedImageCase malformed identical raw operands reject' {
 try{$null=[TerraRendererNativeProof]::ImageDelta([byte[]]@(1,2,3),[byte[]]@(1,2,3),7,5)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
} $true
Case 'DecodedImageCase malformed individual raw image rejects' {
 try{[TerraRendererNativeProof]::Image([byte[]]@(1,2,3),7,5)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
} $true
Case 'DecodedImageCase expected extent cannot be rebound' {
 $p=SaveProof (Clone $originalFrame) (Clone $originalCounters)
 $null=CompareNativeProof $p $p ([pscustomobject]@{width=8;height=5})
} $true
Case 'DecodedImageCase unvalidated raw pixels cannot forge typed proof' {
 $p=SaveProof (Clone $originalFrame) (Clone $originalCounters);$p.image=$image
 $null=CompareNativeProof $p $p $identity.resolution
} $true
Case 'DecodedImageCase missing typed proof rejects' {
 $p=SaveProof (Clone $originalFrame) (Clone $originalCounters);$p.image=$null
 $null=CompareNativeProof $p $p $identity.resolution
} $true
Case 'DecodedImageCase source mutation preserves owned pixels' {
 $bytes=DecodedPng 255 30 20 10;$a=[TerraRendererNativeProof]::DecodeImage($bytes,7,5);$b=[TerraRendererNativeProof]::DecodeImage($bytes,7,5)
 [Array]::Clear($bytes,0,$bytes.Length)
 Require ([TerraRendererNativeProof]::DecodedImageDelta($a,$b,7,5) -eq 0) 'Source mutation changed owned proof'
 Require (@($a.GetType().GetFields()|Where-Object FieldType -eq ([byte[]])).Count -eq 0) 'Mutable pixels exposed'
 Require ($a.GetType().GetConstructors().Length -eq 0) 'Unvalidated public constructor exposed'
}

Write-Output "$passed native verifier unit cases passed; synthetic CPU bytes only"
