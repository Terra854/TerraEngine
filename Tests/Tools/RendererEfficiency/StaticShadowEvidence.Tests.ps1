<# CPU-only P5 receipt tests. AST-extract comparer helpers; never launch an editor.
ReplayOldPolicy preserves the former effective-budget-only admission behavior.
Run: pwsh -NoProfile -File StaticShadowEvidence.Tests.ps1 -WorkRoot <fresh-root>
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WorkRoot,[switch]$ReplayOldPolicy)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($WorkRoot);if(Test-Path -LiteralPath $root){throw 'Fresh WorkRoot required'}
[IO.Directory]::CreateDirectory($root)|Out-Null
$t=$null;$e=$null;$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Compare-RendererEfficiencyEvidence.ps1'),[ref]$t,[ref]$e);if($e.Count){throw 'Comparer parse failure'}
foreach($f in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){Invoke-Expression $f.Extent.Text}
if($ReplayOldPolicy){
	# Exact former P5 budget check; static copy/partition receipts were not read.
	function VerifyNativeStaticShadowExecution($Frame,$Counters){$budget=Metric $Frame.memory.staticCacheBudgetEffectiveBytes budget;$charged=(Metric $Frame.memory.activeBytes active)+(Metric $Frame.memory.reservedBytes reserved)+(Metric $Frame.memory.retiringBytes retiring);Require ($charged -le $budget) 'Static cache admission exceeds effective budget'}
	function VerifyNativeStaticAnimationWindow($Rows,$Envelope){}
	function VerifyNativeStaticRetirementTimeline($Rows){}
	# These independent P5 receipt/recipe guards were absent from the former
	# budget-only path. Replay their absence, not a missing-symbol error.
	function VerifyNativeStaticOptions($Frame,$Document,$Envelope,$Step,$Provenance){}
	function VerifyNativeStaticBudgetLowering($Envelope,$Steps){}
	function VerifyNativeStaticShadowRecipe($Frame,$Counters,$Step){}
	function VerifyNativeSharedStaticRecipe($Frame,$Counters,$Step){}
	function VerifyNativeStaticShadowRegions($Counters,$StepId,$Cameras){}
}
function W($v){[pscustomobject]@{status='Valid';value=$v;reason=''}}
function Clone($v){ConvertTo-Json -InputObject $v -Depth 100 -Compress|ConvertFrom-Json}
function Stamp([int]$id,[bool]$Static){
	$o=[pscustomobject]@{cell=@('0','0','0');offsetMetres=@(0,0,0)}
	$r=[ordered]@{objectId=$id.ToString('x32');lifetimeGeneration='2';segmentId='1';worldPosition=$o;referenceOrigin=$o;rotation=@(1,0,0,0);scale=@(1,1,1);shadowBehavior='0';materialShadowParameters=@(0,0,0,0);texturePublications=@();deformationPalette=@();morphWeights=@();geometryRoute=0;selectedLod=0;selectionFlags=0;firstMeshlet='0';meshletCount='1';firstCluster='0';clusterCount='0';pageId='0';fallbackPageId='0';payloadByteOffset='0';payloadByteSize='0';payloadSource=0;emittedSource=0;staticEligible=$Static;staticClassificationReason=$(if($Static){0}else{1});evidenceMask='511';poseGeneration='4';morphGeneration='4';staticSupport=[pscustomobject]@{authoredStatic=$Static;knownDefaultShadowAbi=$true;ownerPublicationsComplete=$true;materialShadowSupported=$true;geometrySupported=$true;deformationAbsent=$Static}}
	foreach($f in @('meshId','assetId','materialId','shaderId','textureId')){$r[$f]='3'*32}
	if(!$Static){$r.deformationPalette=@(,@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1))}
	[pscustomobject]$r
}
function Fixture([bool]$Reused=$true,[bool]$Cube=$false){
	$s=Stamp 10 $true;$d=Stamp 21 $false;$d.objectId='65000000000000000000000000000021'
	$key=[pscustomobject]@{rendererSessionGeneration='4';viewType='Scene';viewId=1;referenceOrigin=$s.referenceOrigin;lightId=('4'*32);resourceKind=$(if($Cube){1}else{0});faceIndex=0;allocationGeneration='5';regionId='6';extent=@(8,8);depthFormat=126;lightProjectionStamp=[pscustomobject]@{viewProjection=@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1);lightParameters=@(0,0,0,0);shadowFilterParameters=@(0,0,0,0);semanticRevision='2';cubeViewProjections=@();cubeProjectionEvidenceComplete=$Cube}}
	if($Cube){$key.lightProjectionStamp.cubeViewProjections=@(foreach($i in 0..5){,@($key.lightProjectionStamp.viewProjection)})}
	$source=Clone $key;$source.allocationGeneration='8';$source.regionId='9'
	$mask=if($Cube){63}else{1};$submitted=@(if(!$Reused){$s};$d);$draws=$submitted.Count
	$memory=[pscustomobject]@{staticCacheBudgetRequestedBytes=(W 2048);staticCacheBudgetEffectiveBytes=(W 2048);reservedBytes=(W 0);activeBytes=(W 1024);retiringBytes=(W 0);auxiliaryBytes=(W 0);draining=$false}
	$frame=[pscustomobject]@{fixtureStepId='static-dynamic-00';measurementPhase='Measured';stepFrame=60;frameSerial=(W 100);commandSerial=(W 101);submitSerial=(W 102);completionSerial=(W 102);rendererSessionGeneration=(W 4);viewType='Scene';viewId=1;lightId=(W $key.lightId);faceIndex=(W 0);memory=$memory;shadow=[pscustomobject]@{eligibleCasterSegments=(W 2);submittedCasterSegments=(W $draws);actualDrawSubmissions=(W $draws);reuseRegionCount=(W 0);staticReuseRegionCount=(W $(if($Reused){1}else{0}));staticRefreshCasterSegments=(W $(if($Reused){0}else{1}));dynamicRefreshCasterSegments=(W 1)}}
	$c=[pscustomobject]@{fixtureStepId=$frame.fixtureStepId;qualifiedShadowViewKey=$key;eligibleCasterIdentityRows=@($s,$d);submittedCasterIdentityRows=$submitted;nativeCounters=[pscustomobject]@{staticCacheBudgetRequestedBytes=(W 2048);staticCacheBudgetEffectiveBytes=(W 2048);staticReservedBytes=(W 0);staticActiveBytes=(W 1024);staticRetiringBytes=(W 0);staticCacheDraining=$false;staticRefreshes=$frame.shadow.staticRefreshCasterSegments;dynamicRefreshes=$frame.shadow.dynamicRefreshCasterSegments};staticShadowExecution=[pscustomobject]@{available=$true;partitionComplete=$true;copyOverlay=$true;staticReused=$Reused;recordedComplete=$true;requiredFaceMask=$mask;staticRecordedFaceMask=$(if($Reused){0}else{$mask});copiedFaceMask=$mask;staticView=$source;staticContentToken='7';sourceCommandSerial=$(if($Reused){'90'}else{'101'});sourceSubmitSerial=$(if($Reused){'91'}else{'102'});allocationIdentity='9';allocationGeneration='8';backendGeneration='10';actualBytes='1024';staticDraws=$(if($Reused){'0'}else{'1'});dynamicDraws='1';staticMembership=@($s);dynamicMembership=@($d);fallbackReason=''}}
	$c.staticShadowExecution|Add-Member allocations @([pscustomobject]@{sessionGeneration='4';backendGeneration='10';allocationIdentity='9';allocationGeneration='8';actualBytes='1024';depthFormat=126;layers=$(if($Cube){6}else{1});extent=@(8,8);retiring=$false;uses=@()})
	$c.staticShadowExecution|Add-Member completedSubmissions @('91','102')
	$c.staticShadowExecution|Add-Member discardedCommands @()
	$c.staticShadowExecution|Add-Member acceptedSubmissions @()
	@{frame=$frame;counters=$c}
}
$results=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[scriptblock]$Body,[bool]$Reject=$false){$errorText=$null;try{$null=&$Body}catch{$errorText=$_.Exception.Message};$pass=if($Reject){$null -ne $errorText -and ($errorText.StartsWith('NATIVE_FAILED: ') -or $errorText.StartsWith('UNVERIFIED: '))}else{$null -eq $errorText};$results.Add([pscustomobject]@{name=$Name;passed=$pass;error=$errorText});Write-Output ((@('FAIL ','PASS ')[$pass])+$Name);if(!$pass -and $errorText){Write-Output $errorText}}
Case 'reused static submits only actual dynamic contribution' {$f=Fixture;VerifyNativeStaticShadowExecution $f.frame $f.counters}
Case 'fresh static submits both exact categories' {$f=Fixture $false;VerifyNativeStaticShadowExecution $f.frame $f.counters}
Case 'complete cube owns and copies all six faces' {$f=Fixture $true $true;VerifyNativeStaticShadowExecution $f.frame $f.counters}
foreach($field in @('available','partitionComplete','recordedComplete')){Case "missing $field cannot optimize" {$f=Fixture;$f.counters.staticShadowExecution.$field=$false;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true}
Case 'missing raw producer cannot pass' {$f=Fixture;$f.counters.PSObject.Properties.Remove('staticShadowExecution');VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'partition omits static member' {$f=Fixture;$f.counters.staticShadowExecution.staticMembership=@();VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'partition duplicates eligible contribution' {$f=Fixture;$f.counters.staticShadowExecution.dynamicMembership+=Clone $f.counters.staticShadowExecution.staticMembership[0];VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'partition changes actual silhouette' {$f=Fixture;$f.counters.staticShadowExecution.staticMembership[0]=Clone $f.counters.staticShadowExecution.staticMembership[0];$f.counters.staticShadowExecution.staticMembership[0].scale[0]=2;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'dynamic shader cannot be claimed static' {$f=Fixture;$f.counters.staticShadowExecution.staticMembership[0].staticSupport.deformationAbsent=$false;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'cached static contribution cannot execute again' {$f=Fixture;$f.counters.submittedCasterIdentityRows+=Clone $f.counters.staticShadowExecution.staticMembership[0];VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'missing dynamic submission cannot hide behind copy' {$f=Fixture;$f.counters.submittedCasterIdentityRows=@();VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
foreach($field in @('staticContentToken','sourceCommandSerial','sourceSubmitSerial','allocationIdentity','allocationGeneration','backendGeneration','actualBytes')){Case "zero native $field cannot prove copy" {$f=Fixture;$f.counters.staticShadowExecution.$field='0';VerifyNativeStaticShadowExecution $f.frame $f.counters} $true}
Case 'cube five-face copy cannot publish' {$f=Fixture $true $true;$f.counters.staticShadowExecution.copiedFaceMask=31;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'fresh cube five-face static recording cannot publish' {$f=Fixture $false $true;$f.counters.staticShadowExecution.staticRecordedFaceMask=31;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'reused static cannot assert new static recording' {$f=Fixture;$f.counters.staticShadowExecution.staticRecordedFaceMask=1;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'another light cannot authorize depth copy' {$f=Fixture;$f.counters.staticShadowExecution.staticView.lightId='8'*32;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'another projection cannot authorize depth copy' {$f=Fixture;$f.counters.staticShadowExecution.staticView.lightProjectionStamp.viewProjection[0]=2;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'fresh source must join actual current submit' {$f=Fixture $false;$f.counters.staticShadowExecution.sourceSubmitSerial='99';VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'reused static draw scalar must be zero' {$f=Fixture;$f.counters.staticShadowExecution.staticDraws='1';VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'actual dynamic singleton calls cannot use planned count' {$f=Fixture;$f.counters.staticShadowExecution.dynamicDraws='2';VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'raw memory charge cannot disagree with exported metric' {$f=Fixture;$f.counters.nativeCounters.staticActiveBytes.value=512;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'actual owner must remain physically charged' {$f=Fixture;$f.counters.staticShadowExecution.actualBytes='1025';VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'unknown auxiliary GPU allocation cannot be silently free' {$f=Fixture;$f.frame.memory.auxiliaryBytes.value=1;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'budget lowering requires truthful draining state' {$f=Fixture;$f.frame.memory.staticCacheBudgetRequestedBytes.value=0;$f.counters.nativeCounters.staticCacheBudgetRequestedBytes.value=0;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'pending retirement stays charged while draining' {$f=Fixture;$f.frame.memory.staticCacheBudgetRequestedBytes.value=0;$f.counters.nativeCounters.staticCacheBudgetRequestedBytes.value=0;$f.frame.memory.draining=$true;$f.counters.nativeCounters.staticCacheDraining=$true;$f.counters.staticShadowExecution.copyOverlay=$false;$f.frame.shadow.staticReuseRegionCount.value=0;$f.counters.staticShadowExecution.partitionComplete=$false;$f.counters.staticShadowExecution.staticReused=$false;$f.counters.staticShadowExecution.staticDraws='0';$f.counters.staticShadowExecution.dynamicDraws='0';$f.counters.submittedCasterIdentityRows=Clone $f.counters.eligibleCasterIdentityRows;VerifyNativeStaticShadowExecution $f.frame $f.counters}
Case 'combined fallback preserves every eligible contribution' {$f=Fixture;$e=$f.counters.staticShadowExecution;$e.copyOverlay=$false;$f.frame.shadow.staticReuseRegionCount.value=0;$e.partitionComplete=$false;$e.staticReused=$false;$e.staticDraws='0';$e.dynamicDraws='0';$f.counters.submittedCasterIdentityRows=Clone $f.counters.eligibleCasterIdentityRows;$f.frame.shadow.submittedCasterSegments.value=2;$f.frame.shadow.actualDrawSubmissions.value=1;VerifyNativeStaticShadowExecution $f.frame $f.counters}
Case 'combined fallback cannot silently omit eligible member' {$f=Fixture;$f.counters.staticShadowExecution.copyOverlay=$false;$f.frame.shadow.staticReuseRegionCount.value=0;$f.counters.staticShadowExecution.partitionComplete=$false;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
function Window { $rows=@();foreach($i in 60..359){$f=Fixture;$f.frame.stepFrame=$i;$f.frame.frameSerial.value=$i+100;$d=Clone $f.counters.staticShadowExecution.dynamicMembership[0];$d.deformationPalette[0][12]=$i;$f.counters.staticShadowExecution.dynamicMembership=@($d);$f.counters.eligibleCasterIdentityRows[1]=$d;$f.counters.submittedCasterIdentityRows=@($d);$rows+=,$f};return ,$rows }
function AnimationReceipts($Rows){$receipts=@(foreach($i in 60..359){[pscustomobject]@{kind='AnimationPublication';fixtureStepId='static-dynamic-00';objectId='65000000000000000000000000000020';perSample=$true;stepFrame=$i;measurementPhase='Measured';requestedPlaybackTick=31+$i;sampleTick=31+$i;observableTick=31+$i;status='Ok';cpuPosePresent=$true;cpuContentChanged=$true;commandPublicationGeneration=$i+1;authorityPublicationGeneration=$i+1;authoritySessionGeneration=2;snapshotPublicationGeneration=$i+1;previousSnapshotPublicationGeneration=$i}});return [pscustomobject]@{receipts=$receipts}}
function WindowCheck($Rows){VerifyNativeStaticAnimationWindow $Rows (AnimationReceipts $Rows)}
Case '300 actual changing palette samples prove static reuse' {WindowCheck (Window)}
Case 'scalar zero with frozen animation is not SC006 proof' {$r=Window;foreach($f in $r){$f.counters.staticShadowExecution.dynamicMembership[0].deformationPalette[0][12]=60};WindowCheck $r} $true
Case '299 observations cannot replace actual 300 samples' {$r=Window;WindowCheck @($r|Select-Object -First 299)} $true
Case 'static transform change invalidates stable static window' {$r=Window;$r[100].counters.staticShadowExecution.staticMembership[0].scale[0]=2;WindowCheck $r} $true
Case 'inventory omitted charged allocation cannot pass' {$f=Fixture;$f.counters.staticShadowExecution.allocations=@();VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'inventory fabricated extra owner cannot pass' {$f=Fixture;$f.counters.staticShadowExecution.allocations+=Clone $f.counters.staticShadowExecution.allocations[0];VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'inventory wrong session cannot authorize current depth' {$f=Fixture;$f.counters.staticShadowExecution.allocations[0].sessionGeneration='99';VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'inventory copied owner cannot be retiring' {$f=Fixture;$f.counters.staticShadowExecution.allocations[0].retiring=$true;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'inventory negative format or wrong cube layers rejected' {$f=Fixture $true $true;$f.counters.staticShadowExecution.allocations[0].layers=1;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
function Retirement { $a=Fixture;$b=Fixture;$a.frame.frameSerial.value=100;$b.frame.frameSerial.value=101;$a.counters.staticShadowExecution.allocations[0].uses=@([pscustomobject]@{commandSerial='110';submitSerial='111';frameSlot=0});return ,@($a,$b) }
Case 'pending submitted use cannot vanish without completion' {VerifyNativeStaticRetirementTimeline (Retirement)} $true
Case 'actual completed receipt authorizes pending use release' {$r=Retirement;$r[1].counters.staticShadowExecution.completedSubmissions+=,'111';VerifyNativeStaticRetirementTimeline $r}
Case 'pending recorded command cannot vanish without discard' {$r=Retirement;$r[0].counters.staticShadowExecution.allocations[0].uses[0].submitSerial='0';VerifyNativeStaticRetirementTimeline $r} $true
Case 'actual discarded command authorizes recorded use release' {$r=Retirement;$r[0].counters.staticShadowExecution.allocations[0].uses[0].submitSerial='0';$r[1].counters.staticShadowExecution.discardedCommands=@('110');VerifyNativeStaticRetirementTimeline $r}
Case 'accepted recorded command promotes to actual submission' {$r=Retirement;$r[0].counters.staticShadowExecution.allocations[0].uses[0].submitSerial='0';$r[1].counters.staticShadowExecution.allocations[0].uses=@([pscustomobject]@{commandSerial='110';submitSerial='111';frameSlot=0});VerifyNativeStaticRetirementTimeline $r}
Case 'new generation cannot overlap same live allocation handle' {$f=Fixture;$a=Clone $f.counters.staticShadowExecution.allocations[0];$a.allocationGeneration='99';$f.counters.staticShadowExecution.allocations+=,$a;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'static content token cannot change under zero-redraw window' {$r=Window;$r[10].counters.staticShadowExecution.staticContentToken='99';WindowCheck $r} $true
Case 'one completion cannot masquerade as300animationframes' {$r=Window;$r[10].frame.frameSerial.value=$r[9].frame.frameSerial.value;WindowCheck $r} $true
Case 'actual CPU seek receipt missing is not savings proof' {$r=Window;VerifyNativeStaticAnimationWindow $r $null} $true
Case 'seek command tick must equal real measured stepFrame' {$r=Window;$e=AnimationReceipts $r;$e.receipts[100].requestedPlaybackTick=30;VerifyNativeStaticAnimationWindow $r $e} $true
Case 'GPU-only pose cannot stand in for CPU-authoritative fixture' {$r=Window;$e=AnimationReceipts $r;$e.receipts[100].cpuPosePresent=$false;VerifyNativeStaticAnimationWindow $r $e} $true
Case 'negative requested budget cannot drain as native unsigned bytes' {$f=Fixture;$f.frame.memory.staticCacheBudgetRequestedBytes.value=-1;$f.counters.nativeCounters.staticCacheBudgetRequestedBytes.value=-1;$f.frame.memory.draining=$true;$f.counters.nativeCounters.staticCacheDraining=$true;VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'accepted and completed recorded use can retire between observations' {$r=Retirement;$r[0].counters.staticShadowExecution.allocations[0].uses[0].submitSerial='0';$r[1].counters.staticShadowExecution.acceptedSubmissions=@([pscustomobject]@{commandSerial='110';submitSerial='111';frameSlot=0});$r[1].counters.staticShadowExecution.completedSubmissions+=,'111';VerifyNativeStaticRetirementTimeline $r}
Case 'acceptance alone cannot uncharge unfinished submission' {$r=Retirement;$r[0].counters.staticShadowExecution.allocations[0].uses[0].submitSerial='0';$r[1].counters.staticShadowExecution.acceptedSubmissions=@([pscustomobject]@{commandSerial='110';submitSerial='111';frameSlot=0});VerifyNativeStaticRetirementTimeline $r} $true
Case 'accepted mapping cannot use another physical frame slot' {$r=Retirement;$r[0].counters.staticShadowExecution.allocations[0].uses[0].submitSerial='0';$r[1].counters.staticShadowExecution.acceptedSubmissions=@([pscustomobject]@{commandSerial='110';submitSerial='111';frameSlot=1});$r[1].counters.staticShadowExecution.completedSubmissions+=,'111';VerifyNativeStaticRetirementTimeline $r} $true
Case 'duplicate accepted command mapping cannot pass inventory' {$f=Fixture;$map=[pscustomobject]@{commandSerial='110';submitSerial='111';frameSlot=0};$f.counters.staticShadowExecution.acceptedSubmissions=@($map,(Clone $map));VerifyNativeStaticShadowExecution $f.frame $f.counters} $true
Case 'previously observed completion survives bounded history expiry' {$r=Retirement;$r[0].counters.staticShadowExecution.completedSubmissions+=,'111';VerifyNativeStaticRetirementTimeline $r}
Case 'CPU renderer child cannot impersonate Animator seek authority' {$r=Window;$e=AnimationReceipts $r;$e.receipts[100].objectId='65000000000000000000000000000021';VerifyNativeStaticAnimationWindow $r $e} $true
function RegionCameras {return ,@([pscustomobject]@{id='Scene-fixed';viewType='Scene';viewId=1},[pscustomobject]@{id='Game-fixed';viewType='Game';viewId=2})}
function StaticRegions([bool]$Cube=$false,[bool]$Both=$false){$rows=@();foreach($view in $(if($Both){@('Scene','Game')}else{@('Scene')})){foreach($face in $(if($Cube){0..5}else{@(0)})){$f=Fixture $true $Cube;$k=$f.counters.qualifiedShadowViewKey;$k.viewType=$view;$k.viewId=$(if($view -ceq 'Scene'){1}else{2});$k.lightId=$(if($Cube){'65000000000000000000000000000040'}else{'65000000000000000000000000000041'});$k.faceIndex=$face;$f.counters|Add-Member operationId ('a'*32);$f.counters|Add-Member measurementPhase Warmup;$f.counters|Add-Member stepFrame 0;$f.counters|Add-Member readbackProvenance ([pscustomobject]@{frameSerial='100';submitSerial='102'});$rows+=,$f.counters}};return ,$rows}
Case 'dedicated directional step uses one authored actual region' {VerifyNativeStaticShadowRegions (StaticRegions) static-dynamic-00 (RegionCameras)}
Case 'whole cube admission requires all six actual faces' {VerifyNativeStaticShadowRegions (StaticRegions $true) static-dynamic-09 (RegionCameras)}
Case 'retirement churn requires both actual views' {VerifyNativeStaticShadowRegions (StaticRegions $false $true) static-dynamic-13 (RegionCameras)}
Case 'missing cube face cannot satisfy dedicated first cohort' {$r=StaticRegions $true;VerifyNativeStaticShadowRegions @($r|Select-Object -First 5) static-dynamic-09 (RegionCameras)} $true
Case 'observed subset cannot invent expected retirement views' {VerifyNativeStaticShadowRegions (StaticRegions) static-dynamic-13 (RegionCameras)} $true
Case 'another actual light cannot satisfy authored region identity' {$r=StaticRegions;$r[0].qualifiedShadowViewKey.lightId='9'*32;VerifyNativeStaticShadowRegions $r static-dynamic-00 (RegionCameras)} $true
function RecipeFixture { $f=Fixture;$rows=@(foreach($i in 1..1000){[pscustomobject]@{objectId=('6560000000000000'+$i.ToString('x16'))}});$rows+=,[pscustomobject]@{objectId='65000000000000000000000000000021'};$f.counters.eligibleCasterIdentityRows=$rows;$f.frame.memory.staticCacheBudgetRequestedBytes.value=67108864;return $f }
Case 'dedicated source pool proves all actual selected owners' {$f=RecipeFixture;VerifyNativeStaticShadowRecipe $f.frame $f.counters ([pscustomobject]@{action='animate-dynamic-stable-static'})}
Case 'same-count unrelated owner cannot replace authored static pool' {$f=RecipeFixture;$f.counters.eligibleCasterIdentityRows[1].objectId='7'*32;VerifyNativeStaticShadowRecipe $f.frame $f.counters ([pscustomobject]@{action='animate-dynamic-stable-static'})} $true
Case 'authored too-small byte budget cannot be silently raised' {$f=RecipeFixture;VerifyNativeStaticShadowRecipe $f.frame $f.counters ([pscustomobject]@{action='too-small-budget'})} $true
Case 'shared P5 lighting pressure decoder remains allowed' {VerifyNativeLightingStage ([pscustomobject]@{stage='P5';fixtureId='recovery-capacity'})}
Case 'dedicated P5 cannot invent a lighting union' {VerifyNativeLightingStage ([pscustomobject]@{stage='P5';fixtureId='static-dynamic'})} $true
Case 'shared P5 overflow cannot accept unrelated thousand owners' {$f=RecipeFixture;VerifyNativeSharedStaticRecipe $f.frame $f.counters ([pscustomobject]@{action='group-overflow'})} $true
Case 'shared P5 unsupported route cannot accept static actor disguise' {$f=Fixture;VerifyNativeSharedStaticRecipe $f.frame $f.counters ([pscustomobject]@{action='unsupported-grouped-route'})} $true
Case 'reference selected pool keeps actual zero static budget' {$f=RecipeFixture;$f.counters|Add-Member mode Reference;$f.frame.memory.staticCacheBudgetRequestedBytes.value=0;VerifyNativeStaticShadowRecipe $f.frame $f.counters ([pscustomobject]@{action='animate-dynamic-stable-static'})}
Case 'candidate selected pool cannot use reference zero budget' {$f=RecipeFixture;$f.counters|Add-Member mode Candidate;$f.frame.memory.staticCacheBudgetRequestedBytes.value=0;VerifyNativeStaticShadowRecipe $f.frame $f.counters ([pscustomobject]@{action='animate-dynamic-stable-static'})} $true
function BudgetProof {
	$f=Fixture;$f.frame.memory.staticCacheBudgetRequestedBytes.value=1;$f.frame|Add-Member settingsGeneration (W 6);$f.frame|Add-Member effectiveOptions ([pscustomobject]@{shadowReuse='AlwaysRefresh';lightingUploads='Dirty';lightingDepth='Cooperative';shadowSubmission='Grouped';validationPreparation='Serial';staticShadowCacheBudgetBytes=1})
	$start=Clone $f.frame.effectiveOptions;$start.staticShadowCacheBudgetBytes=67108864
	$doc=[pscustomobject]@{identity=[pscustomobject]@{mode='Candidate'};requestedOptions=$start}
	$r=[pscustomobject]@{kind='StaticBudgetPublication';fixtureStepId=$f.frame.fixtureStepId;operationId=('a'*32);requestedGeneration=6;settingsGeneration=6;rendererSessionGeneration=4;optionsSessionGeneration=2;priorOptions=$start;requestedOptions=(Clone $f.frame.effectiveOptions);effectiveOptions=(Clone $f.frame.effectiveOptions)}
	return @{frame=$f.frame;document=$doc;envelope=[pscustomobject]@{operationId=('a'*32);receipts=@($r)};step=[pscustomobject]@{action='too-small-budget'};provenance=[pscustomobject]@{optionsSessionGeneration='2'}}
}
function BudgetCheck($f){VerifyNativeStaticOptions $f.frame $f.document $f.envelope $f.step $f.provenance}
Case 'authored step budget joins exact protected publication' {BudgetCheck (BudgetProof)}
Case 'changed budget without protected publication stays unverified' {$f=BudgetProof;$f.envelope.receipts=@();BudgetCheck $f} $true
Case 'budget request cannot authorize different native settings generation' {$f=BudgetProof;$f.envelope.receipts[0].settingsGeneration=5;BudgetCheck $f} $true
Case 'budget receipt cannot authorize unrelated options lifetime' {$f=BudgetProof;$f.envelope.receipts[0].optionsSessionGeneration=3;BudgetCheck $f} $true
Case 'budget exception never relaxes another renderer option' {$f=BudgetProof;$f.frame.effectiveOptions.lightingUploads='Full';BudgetCheck $f} $true
Case 'queued but not effective budget cannot authorize current frame' {$f=BudgetProof;$f.envelope.receipts[0].requestedOptions.staticShadowCacheBudgetBytes=0;BudgetCheck $f} $true
function LoweringProof {
	$b=BudgetProof;$b.step.action='lower-budget-in-flight';$r=$b.envelope.receipts[0];$r.requestedOptions.staticShadowCacheBudgetBytes=0;$r.effectiveOptions.staticShadowCacheBudgetBytes=0
	$o=[pscustomobject]@{sessionGeneration='4';backendGeneration='10';allocationIdentity='9';allocationGeneration='8';actualBytes='1024';retiring=$false;uses=@([pscustomobject]@{commandSerial='110';submitSerial='111';frameSlot=0})}
	$l=[pscustomobject]@{kind='StaticBudgetLowering';fixtureStepId='static-dynamic-00';stepFrame=1;requestedGeneration=6;priorOptions=(Clone $b.document.requestedOptions);requestedOptions=(Clone $r.requestedOptions);rendererSessionGeneration=4;optionsSessionGeneration=2;allocationIdentity='9';allocationGeneration='8';backendGeneration='10';allocations=@($o);acceptedSubmissions=@();completedSubmissions=@();discardedCommands=@()}
	$b.envelope.receipts+=,$l;$b.step|Add-Member id static-dynamic-00;$observed=Fixture;$observed.frame.stepFrame=0;$b.rows=@($observed);return $b
}
Case 'actual outstanding use and protected zero publication prove lowering' {$f=LoweringProof;VerifyNativeStaticBudgetLowering $f.envelope @($f.step) $f.rows}
Case 'already completed use cannot claim in-flight budget lowering' {$f=LoweringProof;$f.envelope.receipts[1].completedSubmissions=@('111');VerifyNativeStaticBudgetLowering $f.envelope @($f.step) $f.rows} $true
Case 'retired source cannot claim current lower-budget intervention' {$f=LoweringProof;$f.envelope.receipts[1].allocations[0].retiring=$true;VerifyNativeStaticBudgetLowering $f.envelope @($f.step) $f.rows} $true
Case 'old allocation order cannot substitute selected current generation' {$f=LoweringProof;$f.envelope.receipts[1].allocationGeneration='99';$f.envelope.receipts[1].allocations[0].allocationGeneration='99';VerifyNativeStaticBudgetLowering $f.envelope @($f.step) $f.rows} $true
# A matching copied inventory alone is still insufficient without real prior
# native frame ownership. Keep this related negative in the same case group.

Case 'queued lowering without actual protected publication remains unverified' {$f=LoweringProof;$f.envelope.receipts=@($f.envelope.receipts[1]);VerifyNativeStaticBudgetLowering $f.envelope @($f.step) $f.rows} $true
$failed=@($results|Where-Object {!$_.passed}).Count
[pscustomobject]@{schemaVersion=1;scope='Synthetic CPU only';caseCount=$results.Count;failures=$failed;nativeAcceptance=$false;replayOldPolicy=[bool]$ReplayOldPolicy;results=$results}|ConvertTo-Json -Depth 30|Set-Content (Join-Path $root 'result.json')
Write-Output "Renderer efficiency static shadow evidence contracts completed: $($results.Count) cases, $failed failures."
if($failed){exit 1};exit 0
