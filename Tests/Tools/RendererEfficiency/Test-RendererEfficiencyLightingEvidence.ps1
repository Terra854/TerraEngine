<#
.SYNOPSIS
Exercises installed P2 native evidence guards with small synthetic CPU artifacts.
.DESCRIPTION
Run -WorkRoot with a fresh directory and optionally -Comparer. The function AST
is loaded without invoking the comparer CLI or an editor. These fixtures prove
rejection policy and serialization only; they never constitute GPU acceptance.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WorkRoot,[string]$Comparer='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
if(!$Comparer){$Comparer=Join-Path $PSScriptRoot 'Compare-RendererEfficiencyEvidence.ps1'}
$root=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $root){throw 'Lighting evidence tests require a fresh WorkRoot'}
[IO.Directory]::CreateDirectory($root)|Out-Null
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile([IO.Path]::GetFullPath($Comparer),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Comparer syntax error'}
foreach($node in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){Invoke-Expression $node.Extent.Text}
InitializeNativeProof;InitializeNativeLightingProof
$utf8=[Text.UTF8Encoding]::new($false)
function W($value){return [pscustomobject]@{status='Valid';value=$value;reason=''}}
function U(){return [pscustomobject]@{status='Unavailable';value=$null;reason='Lighting has no shadow region'}}
function Clone($value){return ConvertTo-Json -InputObject $value -Depth 100 -Compress|ConvertFrom-Json}
function Upload($purpose){return [pscustomobject]@{purpose=$purpose;payloadLightIds=(W @('65200000000000000000000000000001'));allocationId=(W $(if($purpose -ceq 'FinalLights'){44}else{45}));allocationGeneration=(W 1);allocationSizeBytes=(W 1441792);regionOffsetBytes=(W 0);regionSizeBytes=(W 180224);nonCoherentAtomSize=(W 256);frameSlot=(W 0);snapshotSlot=(W 0);activeRecordCount=(W 1);logicalWriteBytes=(W 0);logicalWriteRecords=(W 0);logicalWriteRanges=(W @());physicalFlushBytes=(W 0);physicalFlushRanges=(W @());mirrorCpuDurationNanoseconds=(W 20);coherent=$true;hostWriteStatus='Succeeded';reason=''}}
$options=[pscustomobject]@{lightingUploads='Dirty';shadowReuse='Enabled';lightingDepth='Cooperative';shadowSubmission='Singleton';staticShadowCacheBudgetBytes=0;validationPreparation='Serial'}
$identity=[pscustomobject]@{fixtureId='lighting-dirty';fixtureHash=(W ('a'*64));runId='lighting-cpu';runIndex=0;seed=65002;mode='Candidate';stage='P2';resolution=[pscustomobject]@{width=2;height=1}}
$frame=[pscustomobject]@{evidenceKind='Lighting';payloadPurpose='FinalLights';fixtureStepId='lighting-dirty-00';stepFrame=60;measurementPhase='Measured';viewType='Scene';viewId=1;lightId=(U);faceIndex=(U);publicationState='Complete';effectiveOptions=$options;lighting=@((Upload FinalLights),(Upload Analysis));shadow=[pscustomobject]@{actualDrawSubmissions=(U)};artifacts=@()}
foreach($pair in @(@('frameSerial',5),@('commandSerial',6),@('submitSerial',7),@('completionSerial',7),@('settingsGeneration',2),@('mutationGeneration',3),@('rendererSessionGeneration',4))){$frame|Add-Member $pair[0] (W $pair[1])}
$counters=[pscustomobject]@{schemaVersion=1;evidenceKind='Lighting';analysisPurpose=$false;fixtureId=$identity.fixtureId;fixtureHash=('a'*64);operationId=('4'*32);fixtureStepId=$frame.fixtureStepId;runId=$identity.runId;runIndex=0;seed=65002;mode='Candidate';stage='P2';measurementPhase='Measured';stepFrame='60';payloadEncoding='float32-ieee-le';recordBytes=176;recordCapacity=1024;payloadPurpose='FinalLights';payloadLightIds=@('65200000000000000000000000000001');lightingUploads=@();colorSourceFormat=44;colorNativeExtent=@(2,1);imageEncoding='png-rgba8';effectiveOptions=$options;nativePreparation=[pscustomobject]@{workerTasks=0;inlineTasks=4;settled=$true};readbackProvenance=[pscustomobject]@{kind='FullNativeSameCommandCompletedReadback';frameSerial='5';commandSerial='6';submitSerial='7';completionSerial='7';settingsGeneration='2';mutationGeneration='3';rendererSessionGeneration='4';fixtureControlGeneration='8';optionsSessionGeneration='9'}}
function Raw($upload){$raw=[ordered]@{purpose=$upload.purpose;available=$true;succeeded=$true;coherent=$upload.coherent;analysisPurpose=($upload.purpose -ceq 'Analysis');reason='';payloadLightIds=$upload.payloadLightIds.value};foreach($name in @('allocationId','allocationGeneration','allocationSizeBytes','regionOffsetBytes','regionSizeBytes','nonCoherentAtomSize','frameSlot','snapshotSlot','activeRecordCount','logicalWriteBytes','logicalWriteRecords','physicalFlushBytes','mirrorCpuDurationNanoseconds')){$raw[$name]=$upload.$name.value};foreach($name in @('logicalWriteRanges','physicalFlushRanges')){$raw[$name]=@($upload.$name.value|ForEach-Object {,@($_.offsetBytes,$_.lengthBytes)})};return [pscustomobject]$raw}
$payload=[byte[]]::new(180224);$payload[0]=1
Add-Type -AssemblyName System.Drawing
$bmp=[Drawing.Bitmap]::new(2,1);$stream=[IO.MemoryStream]::new()
try{$bmp.SetPixel(0,0,[Drawing.Color]::FromArgb(255,20,30,40));$bmp.Save($stream,[Drawing.Imaging.ImageFormat]::Png);$png=$stream.ToArray()}finally{$bmp.Dispose();$stream.Dispose()}
function Save($f,$c,$bytes=$payload){
	if(!(Has $c 'fullSourceCandidateCount')){$c|Add-Member fullSourceCandidateCount $f.fullSourceCandidateCount.value}
	$script:saveOrdinal++;$c.lightingUploads=@($f.lighting|ForEach-Object {Raw $_})
	$a=[Collections.Generic.List[object]]::new()
	foreach($part in @(@('LightingPayload','payload.lighting.bin',$bytes),@('Image','image.png',$png),@('Counters','proof.counters.json',$utf8.GetBytes((Canonical $c))))){$relative="$script:saveOrdinal-$($part[1])";$path=Join-Path $root $relative;[IO.File]::WriteAllBytes($path,[byte[]]$part[2]);$item=[pscustomobject]@{kind=$part[0];status='Valid';path=$relative;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();reason=''};if($part[0] -ceq 'LightingPayload'){$item|Add-Member payloadPurpose $f.payloadPurpose};$a.Add($item)}
	$f.artifacts=$a.ToArray();return ReadNativeLightingProof $f $identity $root (NewNativeArtifactClosureIndex $f.artifacts)
}
# Complete frame-shaped fixtures exercise the same authoritative schema used by
# nested baseline receipts, including explicitly unavailable unrelated metrics.
$schemaPath=Join-Path (Split-Path $Comparer) 'Schemas/evidence.schema.json'
$script:definitionRoot=Get-Content -LiteralPath $schemaPath -Raw|ConvertFrom-Json
foreach($name in @('eligibleCasterSegments','submittedCasterSegments','actualDrawSubmissions','groups','singletons','staticRefreshCasterSegments','dynamicRefreshCasterSegments','reuseRegionCount','staticReuseRegionCount','contentOwner','refreshReason','fallbackReason')){if(!(Has $frame.shadow $name)){$frame.shadow|Add-Member $name (U)}}
$frame|Add-Member fullSourceCandidateCount (W 1)
$frame|Add-Member memory ([pscustomobject]@{})
foreach($name in @('staticCacheBudgetRequestedBytes','staticCacheBudgetEffectiveBytes','reservedBytes','activeBytes','retiringBytes','auxiliaryBytes','admissionReason','fallbackReason')){$frame.memory|Add-Member $name (U)}
$frame.memory|Add-Member draining 'Unavailable';$frame|Add-Member depthStageTimingNanoseconds (U);$frame|Add-Member totalFrameTimingNanoseconds (U)
$saveOrdinal=0
$results=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[scriptblock]$Body,[bool]$Reject=$false){$caught=$null;try{$null=&$Body}catch{$caught=$_.Exception.Message};$pass=if($Reject){$null -ne $caught -and ($caught.StartsWith('NATIVE_FAILED: ') -or $caught.StartsWith('UNVERIFIED: ') -or $caught.StartsWith('$.' ))}else{$null -eq $caught};$results.Add([pscustomobject]@{name=$Name;passed=$pass;rejection=$caught});$label=if($pass){'PASS '}else{'FAIL '};Write-Output ($label+$Name);if(!$pass -and $caught){Write-Output $caught}}
Case 'complete synthetic full native lighting proof' {Save (Clone $frame) (Clone $counters)}
Case 'short full payload rejected' {Save (Clone $frame) (Clone $counters) ([byte[]]::new(176))} $true
Case 'nonzero inactive tail rejected' {$b=$payload.Clone();$b[176]=1;Save (Clone $frame) (Clone $counters) $b} $true
Case 'signed zero and NaN bits preserved without float normalization' {$b=$payload.Clone();[Array]::Copy([BitConverter]::GetBytes([uint32]2147483648),0,$b,4,4);[Array]::Copy([BitConverter]::GetBytes([uint32]2143363909),0,$b,8,4);$p=Save (Clone $frame) (Clone $counters) $b;NativeRequire ([TerraRendererLightingProof]::Dirty($payload,$p.payload).Length -eq 1) 'IEEE bits lost'}
# Case runs each body immediately. Keep the body in this script's scope so a
# child-script invocation via -Command can still find Clone and Save helpers.
# Use a distinct loop variable: Case's Name parameter must not shadow it.
foreach($receiptField in @('frameSerial','commandSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','fixtureControlGeneration','optionsSessionGeneration')){
	Case "zero actual $receiptField rejected" {
		$c=Clone $counters
		$c.readbackProvenance.$receiptField='0'
		Save (Clone $frame) $c
	} $true
}
Case 'unknown payload ABI rejected' {$c=Clone $counters;$c.recordBytes=160;Save (Clone $frame) $c} $true
Case 'unknown encoding rejected' {$c=Clone $counters;$c.payloadEncoding='float32';Save (Clone $frame) $c} $true
Case 'forged shadow identity rejected' {$f=Clone $frame;$f.lightId=W ('1'*32);Save $f (Clone $counters)} $true
Case 'wrong payload purpose rejected' {$c=Clone $counters;$c.payloadPurpose='Analysis';Save (Clone $frame) $c} $true
Case 'missing second purpose rejected' {$f=Clone $frame;$f.lighting=@($f.lighting[0]);Save $f (Clone $counters)} $true
Case 'duplicate purpose rejected' {$f=Clone $frame;$f.lighting[1].purpose='FinalLights';Save $f (Clone $counters)} $true
Case 'slot overflow rejected' {$f=Clone $frame;$f.lighting[0].snapshotSlot=W 8;Save $f (Clone $counters)} $true
Case 'active count over capacity rejected' {$f=Clone $frame;$f.lighting[0].activeRecordCount=W 1025;Save $f (Clone $counters)} $true
Case 'missing actual selected IDs rejected' {$f=Clone $frame;$f.lighting[0].payloadLightIds=W @();Save $f (Clone $counters)} $true
Case 'failed host upload rejected' {$f=Clone $frame;$f.lighting[0].hostWriteStatus='Failed';Save $f (Clone $counters)} $true
Case 'coherent no-op needs no flush' {VerifyNativeLightingRanges (Upload FinalLights)}
function OneWrite(){ $u=Upload FinalLights;$u.coherent=$false;$u.logicalWriteBytes=W 176;$u.logicalWriteRecords=W 1;$u.logicalWriteRanges=W @([pscustomobject]@{offsetBytes=0;lengthBytes=176});$u.physicalFlushBytes=W 256;$u.physicalFlushRanges=W @([pscustomobject]@{offsetBytes=0;lengthBytes=256});return $u }
Case 'one record and atom overhead accepted' {VerifyNativeLightingRanges (OneWrite)}
Case 'unbounded flush rejected' {$u=OneWrite;$u.physicalFlushRanges.value[0].lengthBytes=1442048;VerifyNativeLightingRanges $u} $true
Case 'coherent invented flush rejected' {$u=OneWrite;$u.coherent=$true;VerifyNativeLightingRanges $u} $true
Case 'mismatched logical byte charge rejected' {$u=OneWrite;$u.logicalWriteBytes=W 177;VerifyNativeLightingRanges $u} $true
Case 'missing complete native closure rejected' {$p=Save (Clone $frame) (Clone $counters);ReadNativeLightingProof $p.frame $identity $root (NewNativeArtifactClosureIndex @())} $true
Case 'real canonical dirty record requires exact write' {$a=@{payload=$payload};$b=$payload.Clone();$b[0]=2;$p=@{payload=$b;upload=(OneWrite)};CompareNativeLightingUpload $a $p}
Case 'zero counter cannot conceal changed payload' {$a=@{payload=$payload};$b=$payload.Clone();$b[0]=2;CompareNativeLightingUpload $a @{payload=$b;upload=(Upload FinalLights)}} $true
Case 'same bytes do not authorize invented logical write' {CompareNativeLightingUpload @{payload=$payload} @{payload=$payload;upload=(OneWrite)}} $true
# Reuse the accepted production proof only as input to cohort policy; the
# fixture explicitly remains a CPU model of metadata, not a native submission.
function Cohort($index,$purpose){
	$f=Clone $frame;$c=Clone $counters;$f.payloadPurpose=$purpose;$c.fixtureId=$identity.fixtureId;$c.payloadPurpose=$purpose;$c.analysisPurpose=$purpose -ceq 'Analysis'
	$f.stepFrame=$index;$f.measurementPhase='LightingBaseline';$c.stepFrame=[string]$index;$c.measurementPhase='LightingBaseline'
	foreach($field in @('frameSerial','commandSerial','submitSerial','completionSerial')){$f.$field=W (100+$index);$c.readbackProvenance.$field=[string](100+$index)}
	foreach($u in $f.lighting){$slot=$index%8;$u.frameSlot=W ([int][Math]::Floor($index/8)%3);$u.snapshotSlot=W $slot;$u.regionOffsetBytes=W ($slot*180224)}
	return Save $f $c
}
$cameras=@([pscustomobject]@{viewType='Scene';viewId=1})
$cohort=[Collections.Generic.List[object]]::new()
foreach($i in 0..23){foreach($purpose in @('FinalLights','Analysis')){$cohort.Add((Cohort $i $purpose))}}
Case 'all24 physical slots require both actual purposes' {VerifyNativeLightingCohorts $cohort.ToArray() $cameras $true}
Case 'missing purpose cannot pass observed slots' {VerifyNativeLightingCohorts @($cohort.ToArray()|Where-Object {$_.frame.payloadPurpose -ceq 'FinalLights'}) $cameras $true} $true
Case 'missing physical frame slot cannot pass coverage' {VerifyNativeLightingCohorts @($cohort.ToArray()|Where-Object {$_.upload.frameSlot.value -ne 2}) $cameras $true} $true
Case 'undeclared view cannot replace missing declared output' {$copy=Clone $cohort[0];$copy.frame.viewType='Game';VerifyNativeLightingCohorts @($copy,$cohort[1]) $cameras} $true
Case 'mixed actual control generation rejected withincohort' {$copy=Clone $cohort[1];$copy.counters.readbackProvenance.fixtureControlGeneration='99';VerifyNativeLightingCohorts @($cohort[0],$copy) $cameras} $true
Case 'mixed effective options rejected withincohort' {$copy=Clone $cohort[1];$copy.frame.effectiveOptions.lightingUploads='Full';VerifyNativeLightingCohorts @($cohort[0],$copy) $cameras} $true
Case 'sameview contradictory command rejected' {$copy=Clone $cohort[1];$copy.counters.readbackProvenance.commandSerial='99';VerifyNativeLightingCohorts @($cohort[0],$copy) $cameras} $true
Case 'duplicate purpose rejected withincohort' {VerifyNativeLightingCohorts @($cohort[0],$cohort[0],$cohort[1]) $cameras} $true
Case 'same payload different selected IDs rejects paired proof' {$a=Save (Clone $frame) (Clone $counters);$b=@{};foreach($key in $a.Keys){$b[$key]=$a[$key]};$b.payloadLightIds=@('65200000000000000000000000000002');CompareNativeLightingProof $a $b $identity.resolution} $true
Case 'frozen reverse cannot use original identity order' {$p=@{frame=[pscustomobject]@{payloadPurpose='FinalLights';fullSourceCandidateCount=(W 1000)};payloadLightIds=@('65200000000000000000000000000001')};VerifyNativeLightingSelection $p ([pscustomobject]@{action='reorder-lights'}) 'lighting-dirty'} $true
Case 'unknown predecessor sampling is not fake exact write proof' {$p=Save (Clone $frame) (Clone $counters);$previous=@{};foreach($key in $p.Keys){$previous[$key]=$p[$key]};$previous.frame=Clone $p.frame;$previous.frame.frameSerial=W 4;$previous.payload=$payload.Clone();$previous.payload[0]=2;$state=@{'lighting-dirty-00'=@{owners=@{};slots=@{};firstChanges=@{}}};$state['lighting-dirty-00'].owners[$p.owner]=$previous;UpdateNativeLightingTimeline $state $p ([pscustomobject]@{id='lighting-dirty-00';action='stable-all-slots'})}

# Exercise actual receipt readers with complete nested frame-shaped artifacts.
# Source counts here are tiny CPU inputs; no timing or GPU claim is emitted.
$identity.fixtureId='multiview-lights'
$baselineProofs=[Collections.Generic.List[object]]::new()
foreach($i in 0..59){foreach($purpose in @('FinalLights','Analysis')){$baselineProofs.Add((Cohort $i $purpose))}}
$baselineFrames=@($baselineProofs.ToArray()|ForEach-Object frame)
$baselineArtifacts=@($baselineFrames|ForEach-Object artifacts)
$baselineReceipt=[pscustomobject]@{kind='LightingBaseline';fixtureStepId=$frame.fixtureStepId;frames=$baselineFrames;artifacts=$baselineArtifacts}
$baselineEnvelope=[pscustomobject]@{operationId=$counters.operationId;receipts=@($baselineReceipt)}
$baselineDocument=[pscustomobject]@{artifacts=$baselineArtifacts;frames=@();requestedOptions=$options}
$step=[pscustomobject]@{id=$frame.fixtureStepId;action='stable-all-slots';nativePayload=[pscustomobject]@{baselineCandidateOrder=@('65200000000000000000000000000001');candidateOrder=@('65200000000000000000000000000001')}}
Case 'full nested baseline keeps actual last owner bytes' {$script:acceptedBaselineState=ReadNativeLightingBaselines $baselineEnvelope $baselineDocument $identity $root @($step) $cameras;NativeRequire ($script:acceptedBaselineState[$step.id].owners.Count -eq 48) 'Baseline owner set incomplete'}
function TestBaselineState(){
	# Reuse fixture inputs already accepted by the real full receipt reader. Only
	# mutable timeline maps are copied; native parity still freshly hashes bytes.
	$owners=@{};foreach($key in $script:acceptedBaselineState[$step.id].owners.Keys){$owners[$key]=$script:acceptedBaselineState[$step.id].owners[$key]}
	return @{$step.id=@{owners=$owners;slots=@{};firstChanges=@{};baseline=$true;deviceAuthority=$false}}
}
Case 'baseline parity freshly reads paired full payloads' {$state=TestBaselineState;NativeRequire ((CompareNativeLightingBaselines $state $state $identity $identity $root $root) -eq 0) 'Identical baseline differs'}
Case 'baseline changed bytes cannot pass paired parity' {$state=TestBaselineState;$other=@{$step.id=@{owners=@{}}};foreach($key in $state[$step.id].owners.Keys){$other[$step.id].owners[$key]=$state[$step.id].owners[$key]};$key=@($state[$step.id].owners.Keys)[0];$f=Clone $state[$step.id].owners[$key].frame;$artifact=NativeArtifact $f Counters $root;$c=$utf8.GetString($artifact.bytes)|ConvertFrom-Json;$bytes=$payload.Clone();$bytes[0]=2;$changed=Save $f $c $bytes;$other[$step.id].owners[$key]=@{frame=$changed.frame;upload=$changed.upload};CompareNativeLightingBaselines $state $other $identity $identity $root $root} $true
Case 'missing authoritative baseline cannot pass' {$e=Clone $baselineEnvelope;$e.receipts=@();ReadNativeLightingBaselines $e $baselineDocument $identity $root @($step) $cameras} $true
Case 'missing baseline receipt closure cannot pass' {$e=Clone $baselineEnvelope;$e.receipts[0].artifacts=@();ReadNativeLightingBaselines $e $baselineDocument $identity $root @($step) $cameras} $true
Case 'missing root baseline closure cannot pass' {$d=Clone $baselineDocument;$d.artifacts=@();ReadNativeLightingBaselines $baselineEnvelope $d $identity $root @($step) $cameras} $true

function Transition($purpose){
	$f=Clone $frame;$c=Clone $counters;$c.fixtureId=$identity.fixtureId;$f.payloadPurpose=$purpose;$c.payloadPurpose=$purpose;$c.analysisPurpose=$purpose -ceq 'Analysis';$f.measurementPhase='Transition';$c.measurementPhase='Transition';$f.stepFrame=0;$c.stepFrame='0'
	foreach($pair in @(@('frameSerial',200),@('commandSerial',201),@('submitSerial',202),@('completionSerial',202))){$f.($pair[0])=W $pair[1];$c.readbackProvenance.($pair[0])=[string]$pair[1]}
	return Save $f $c
}
$transitions=@((Transition FinalLights),(Transition Analysis));$transitionArtifacts=@($transitions|ForEach-Object {$_.frame.artifacts})
$transitionReceipt=[pscustomobject]@{kind='ControlTransition';fixtureStepId=$step.id;action=$step.action;controlGeneration='8';originalCorrelation=[pscustomobject]@{operationId=$counters.operationId;fixtureStepId=$step.id;stepFrame=0;settingsGeneration=2;mutationGeneration=3;rendererSessionGeneration=4};frameSerial='200';commandSerial='201';submitSerial='202';completionSerial='202';frames=@($transitions|ForEach-Object frame);artifacts=$transitionArtifacts}
$transitionEnvelope=[pscustomobject]@{operationId=$counters.operationId;receipts=@($baselineReceipt,$transitionReceipt)}
$warmup=Clone $frame;$warmup.measurementPhase='Warmup';$warmup.stepFrame=0;$warmup.frameSerial=W 300
$transitionDocument=[pscustomobject]@{frames=@($warmup);artifacts=@($baselineArtifacts)+@($transitionArtifacts);requestedOptions=$options}
Case 'retained native transition follows last baseline before warmup' {$state=TestBaselineState;$map=ReadNativeLightingTransitions $transitionEnvelope $transitionDocument $identity $root @($step) $cameras $state;NativeRequire ($map[$step.id].Count -eq 2) 'Transition purposes omitted'}
Case 'later hit cannot replace original transition chronology' {$e=Clone $transitionEnvelope;$e.receipts[1].frameSerial='301';$state=@{};ReadNativeLightingTransitions $e $transitionDocument $identity $root @($step) $cameras $state} $true
Case 'missing actual lighting transition stays unavailable' {$e=Clone $transitionEnvelope;$e.receipts=@($baselineReceipt);ReadNativeLightingTransitions $e $transitionDocument $identity $root @($step) $cameras @{}} $true

Case 'new device owner without actual receipt is rejected' {$deviceStep=[pscustomobject]@{id=$step.id;action='device-recovery'};ReadNativeLightingBaselines $baselineEnvelope $baselineDocument $identity $root @($deviceStep) $cameras} $true
Case 'authorized new device owner requires full initialization' {$p=Transition FinalLights;$p.frame.measurementPhase='Warmup';$p.upload.logicalWriteBytes=W 180224;$p.upload.logicalWriteRecords=W 1024;$p.upload.logicalWriteRanges=W @([pscustomobject]@{offsetBytes=0;lengthBytes=180224});$state=@{$step.id=@{owners=@{};slots=@{};firstChanges=@{};deviceAuthority=$true;deviceSession=4;deviceOptionsEpoch=9;baselineFrame=148;deviceAllocations=@{}}};UpdateNativeLightingTimeline $state $p ([pscustomobject]@{id=$step.id;action='device-recovery'});NativeRequire ($state[$step.id].owners.ContainsKey($p.owner)) 'Initialized device owner not retained'}

Case 'baseline native options must equal requested options' {$d=Clone $baselineDocument;$d.requestedOptions.lightingUploads='Full';ReadNativeLightingBaselines $baselineEnvelope $d $identity $root @($step) $cameras} $true
Case 'transition native options must equal requested options' {$d=Clone $transitionDocument;$d.requestedOptions.lightingUploads='Full';$state=TestBaselineState;ReadNativeLightingTransitions $transitionEnvelope $d $identity $root @($step) $cameras $state} $true
Case 'different qualified views cannot alias payload regions' {$game=@();foreach($p in @($cohort[0],$cohort[1])){$g=Clone $p;$g.frame.viewType='Game';$g.frame.viewId=2;$g.upload.frameSlot=W 1;$game+=,$g};VerifyNativeLightingCohorts @($cohort[0],$cohort[1],$game[0],$game[1]) @($cameras[0],[pscustomobject]@{viewType='Game';viewId=2})} $true

Case 'valid device authority cannot authorize unrelated session' {$p=Transition FinalLights;$p.frame.rendererSessionGeneration=W 99;$p.upload.logicalWriteBytes=W 180224;$p.upload.logicalWriteRecords=W 1024;$p.upload.logicalWriteRanges=W @([pscustomobject]@{offsetBytes=0;lengthBytes=180224});$state=@{$step.id=@{owners=@{};slots=@{};firstChanges=@{};deviceAuthority=$true;deviceSession=5;deviceOptionsEpoch=9;baselineFrame=148;deviceAllocations=@{}}};UpdateNativeLightingTimeline $state $p ([pscustomobject]@{id=$step.id;action='device-recovery'})} $true

Case 'old baseline owner cannot masquerade as recovered measured owner' {$p=Transition FinalLights;$p.frame.measurementPhase='Measured';$p.frame.rendererSessionGeneration=W 4;$state=@{$step.id=@{owners=@{};slots=@{};firstChanges=@{};deviceAuthority=$true;deviceSession=5;deviceOptionsEpoch=10;baselineFrame=148;deviceAllocations=@{}}};$state[$step.id].owners[$p.owner]=$p;UpdateNativeLightingTimeline $state $p ([pscustomobject]@{id=$step.id;action='device-recovery'})} $true

Case 'unobserved device initialization cannot become acceptance' {$p=Transition FinalLights;$state=@{$step.id=@{owners=@{};slots=@{};firstChanges=@{};deviceAuthority=$true;deviceSession=4;deviceOptionsEpoch=9;baselineFrame=148;deviceAllocations=@{}}};try{UpdateNativeLightingTimeline $state $p ([pscustomobject]@{id=$step.id;action='device-recovery'})}catch{if(!$_.Exception.Message.StartsWith('UNVERIFIED:')){throw 'Missing initialization must remain Unverified'};throw}} $true

Case 'uncapped source count must match raw and frame' {$f=Clone $frame;$c=Clone $counters;$c.fixtureId=$identity.fixtureId;$f.fullSourceCandidateCount=W 2;$c|Add-Member fullSourceCandidateCount 1;Save $f $c} $true
Case 'uncapped source count cannot be unavailable' {$f=Clone $frame;$f.fullSourceCandidateCount=U;$c=Clone $counters;$c.fixtureId=$identity.fixtureId;Save $f $c} $true
Case 'uncapped source count cannot be below payload count' {$f=Clone $frame;$f.fullSourceCandidateCount=W 0;$c=Clone $counters;$c.fixtureId=$identity.fixtureId;Save $f $c} $true
Case 'uncapped source count cannot exceed owned bound' {$f=Clone $frame;$f.fullSourceCandidateCount=W 4097;$c=Clone $counters;$c.fixtureId=$identity.fixtureId;Save $f $c} $true
Case 'actual1025 source can produce capped1024 payload' {$f=Clone $frame;$c=Clone $counters;$c.fixtureId=$identity.fixtureId;$f.fullSourceCandidateCount=W 1025;foreach($u in $f.lighting){$u.activeRecordCount=W 1024;$u.payloadLightIds=W @(1..1024|ForEach-Object {'6520000000000000'+$_.ToString('x16')})};$p=Save $f $c ([byte[]]::new(180224));VerifyNativeLightingSourceCount $p ([pscustomobject]@{action='over-cap-1025'}) 'lighting-thresholds';NativeRequire ((Metric $p.upload.activeRecordCount activeRecordCount) -eq 1024) 'Payload cap differs'}
Case 'capped IDs and shared omissions cannot manufacture complete source proof' {
	$rejected=0
	$p=@{frame=[pscustomobject]@{fullSourceCandidateCount=(W 1024)}}
	try{VerifyNativeLightingSourceCount $p ([pscustomobject]@{action='over-cap-1025'}) 'lighting-thresholds'}catch{if(!$_.Exception.Message.StartsWith('NATIVE_FAILED:')){throw};$rejected++}
	foreach($pair in @(@(5,0,'stable-all-slots'),@(1025,1,'buffer-overflow'))){
		$p=@{frame=[pscustomobject]@{fullSourceCandidateCount=(W $pair[0])};upload=[pscustomobject]@{activeRecordCount=(W $pair[1])}}
		try{VerifyNativeLightingSelection $p ([pscustomobject]@{action=$pair[2]}) 'recovery-capacity'}catch{if(!$_.Exception.Message.StartsWith('NATIVE_FAILED:')){throw};$rejected++}
	}
	$p=@{frame=[pscustomobject]@{fullSourceCandidateCount=(W 5)};upload=[pscustomobject]@{activeRecordCount=(W 0)}}
	try{VerifyNativeLightingSourceCount $p ([pscustomobject]@{action='stable-all-slots'}) 'recovery-capacity' $true}catch{if(!$_.Exception.Message.StartsWith('NATIVE_FAILED:')){throw};$rejected++}
	$p.upload.activeRecordCount=W 5
	VerifyNativeLightingSourceCount $p ([pscustomobject]@{action='stable-all-slots'}) 'recovery-capacity' $true
	NativeRequire ($rejected -eq 4) 'Missing source count or shared baseline/post-action omission was accepted'
	foreach($pair in @(@(5,5,'stable-all-slots'),@(1025,1024,'buffer-overflow'))){
		$p=@{frame=[pscustomobject]@{fullSourceCandidateCount=(W $pair[0])};upload=[pscustomobject]@{activeRecordCount=(W $pair[1])}}
		VerifyNativeLightingSelection $p ([pscustomobject]@{action=$pair[2]}) 'recovery-capacity'
	}
}

$failed=@($results|Where-Object {!$_.passed}).Count
[IO.File]::WriteAllText((Join-Path $root 'result.json'),(Canonical ([ordered]@{schemaVersion=1;scope='Synthetic CPU verifier contracts only';gpuAcceptance=$false;cases=$results.Count;failed=$failed;results=$results.ToArray()})),$utf8)
Write-Output "Renderer efficiency lighting evidence contracts completed: $($results.Count) cases, $failed failures."
if($failed){exit 1};exit 0
