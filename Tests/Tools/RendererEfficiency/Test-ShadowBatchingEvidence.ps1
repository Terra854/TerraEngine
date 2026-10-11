<#
.SYNOPSIS
CPU-only native shadow batching contract regressions; no editor or GPU execution.
.DESCRIPTION
Run with a fresh WorkRoot. Production comparer helpers are AST-extracted. The
explicit replay switch reconstructs the former aggregate-only P4 admission,
which did not examine actual source/group/singleton receipts. It is not GPU proof.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WorkRoot,[string]$Comparer='', [switch]$ReplayLegacyShadowBatchAdmission)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if(!$Comparer){$Comparer=Join-Path $PSScriptRoot 'Compare-RendererEfficiencyEvidence.ps1'}
$root=[IO.Path]::GetFullPath($WorkRoot);if(Test-Path -LiteralPath $root){throw 'Shadow batch contracts need a fresh WorkRoot'}
[IO.Directory]::CreateDirectory($root)|Out-Null
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($Comparer,[ref]$tokens,[ref]$errors);if($errors.Count){throw 'Comparer parse failure'}
foreach($node in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){Invoke-Expression $node.Extent.Text}
if($ReplayLegacyShadowBatchAdmission){
	# Former P4 logic used only aggregate metric sums. ReadNativeProof still
	# checked pixels/eligible rows, but no native batch execution receipt existed.
	function VerifyNativeShadowBatchExecution($Frame,$Counters){}
	function VerifyNativeShadowBatchRecipe($Frame,$Counters,$Step){}
	function VerifyNativeShadowBatchRegions($Counters,[string]$StepId,[string]$FixtureId='shadow-batching',$Cameras=@()){VerifyNativeBundle $Counters}
	function VerifyNativeSharedShadowBatchRecipe($Frame,$Counters,$Step,[string]$FixtureId){}
	function VerifyNativeP4SharedCohorts($Rows,$Cameras,[string]$FixtureId){}
	function VerifyNativeShadowBatchReduction($Candidate,$Reference,[int]$MinimumCasters=1000){Require ((Metric $Candidate.shadow.eligibleCasterSegments eligible) -ge 1000 -and (Metric $Reference.shadow.actualDrawSubmissions draws) -gt 0 -and (Metric $Candidate.shadow.actualDrawSubmissions draws) -le (Metric $Reference.shadow.actualDrawSubmissions draws)*0.1) 'Old aggregate shadow reduction'}
}
function W($v){return [pscustomobject]@{status='Valid';value=$v;reason=''}}
function Clone($v){return ConvertTo-Json -InputObject $v -Depth 100 -Compress|ConvertFrom-Json}
function Member([int]$i=1){return [pscustomobject]@{objectId=('6500000000000000000000000000'+$i.ToString('D4'));lifetimeGeneration='9';segmentId=([string]$i)}}
function Row([int]$i=1){$r=Member $i;$r|Add-Member firstMeshlet 0;$r|Add-Member meshletCount 4;return $r}
function Source([int]$i=1){$m=Member $i;return [pscustomobject]@{objectId=$m.objectId;lifetimeGeneration=$m.lifetimeGeneration;sourceOrdinal=($i-1);ownerFirstMeshlet=0;ownerMeshletCount=4;resolved=$true;excludedByMaterial=$false;cameraVisible=$true}}
function Fixture([bool]$Singleton=$false){
	$groups=@();$singletons=@();if($Singleton){$singletons=@((Member 1),(Member 2))}else{$groups=@([pscustomobject]@{firstInstance=0;instanceCount=2;taskGroupsX=2;pipelineIdentity='71';layoutIdentity='72';members=@((Member 1),(Member 2))})}
	$shadow=[pscustomobject]@{eligibleCasterSegments=(W 2);submittedCasterSegments=(W 2);actualDrawSubmissions=(W $(if($Singleton){2}else{1}));groups=(W $groups.Count);singletons=(W $singletons.Count);reuseRegionCount=(W 0)}
	$frame=[pscustomobject]@{effectiveOptions=[pscustomobject]@{shadowReuse='AlwaysRefresh';shadowSubmission=$(if($Singleton){'Singleton'}else{'Grouped'})};shadow=$shadow}
	$counters=[pscustomobject]@{readbackProvenance=[pscustomobject]@{fixtureControlGeneration='4';optionsSessionGeneration='5'};eligibleCasterIdentityRows=@((Row 1),(Row 2));submittedCasterIdentityRows=@((Row 1),(Row 2));nativeCounters=(Clone $shadow);shadowBatchExecution=[pscustomobject]@{available=$true;recordedComplete=$true;frameSlotSafe=$true;uploadAttempted=(!$Singleton);uploadSucceeded=(!$Singleton);allocationIdentity='21';allocationGeneration='22';backendGeneration='23';instanceBytes='352';frameSlot=0;snapshotSlot=0;sources=@((Source 1),(Source 2));groups=$groups;singletons=$singletons}}
	return [pscustomobject]@{frame=$frame;counters=$counters}
}
$results=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[scriptblock]$Body,[bool]$Reject=$false){$caught=$null;try{$null=&$Body}catch{$caught=$_.Exception.Message};$pass=if($Reject){$null -ne $caught -and ($caught.StartsWith('NATIVE_FAILED: ') -or $caught.StartsWith('UNVERIFIED: '))}else{$null -eq $caught};$results.Add([pscustomobject]@{name=$Name;passed=$pass;error=$caught});Write-Output ((@('FAIL ','PASS ')[$pass])+$Name);if(!$pass -and $caught){Write-Output $caught}}
Case 'actual two-member group covers both source owners' {$f=Fixture;VerifyNativeShadowBatchExecution $f.frame $f.counters}
Case 'actual singleton reference covers both source owners' {$f=Fixture $true;VerifyNativeShadowBatchExecution $f.frame $f.counters}
foreach($flag in @('available','recordedComplete','frameSlotSafe')){Case "missing $flag cannot pass" {$f=Fixture;$f.counters.shadowBatchExecution.$flag=$false;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true}
Case 'missing execution receipt is unverified' {$f=Fixture;$f.counters.PSObject.Properties.Remove('shadowBatchExecution');VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'unresolved off-camera source cannot disappear' {$f=Fixture;$f.counters.shadowBatchExecution.sources[1].resolved=$false;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'omitted off-camera eligible member fails full source proof' {$f=Fixture;$f.counters.eligibleCasterIdentityRows=@($f.counters.eligibleCasterIdentityRows[0]);VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'explicit material-excluded source must not execute' {$f=Fixture;$f.counters.shadowBatchExecution.sources[1].excludedByMaterial=$true;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'source meshlet gap rejects otherwise matching execution' {$f=Fixture;$f.counters.eligibleCasterIdentityRows[1].meshletCount=3;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'duplicate source ordinal rejected' {$f=Fixture;$f.counters.shadowBatchExecution.sources[1].sourceOrdinal=0;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'selected source owner overflow rejected' {$f=Fixture;$f.counters.shadowBatchExecution.sources[0].ownerFirstMeshlet=[uint32]::MaxValue;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'duplicate actual group member rejected' {$f=Fixture;$f.counters.shadowBatchExecution.groups[0].members[1]=Clone $f.counters.shadowBatchExecution.groups[0].members[0];VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'actual command count cannot use planned groups' {$f=Fixture;$f.frame.shadow.groups.value=2;$f.counters.nativeCounters.groups.value=2;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'unknown native pipeline identity rejected' {$f=Fixture;$f.counters.shadowBatchExecution.groups[0].pipelineIdentity='0';VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'instance ABI byte extent is exact and bounded' {$f=Fixture;$f.counters.shadowBatchExecution.instanceBytes='351';VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'actual group range cannot exceed native stream' {$f=Fixture;$f.counters.shadowBatchExecution.groups[0].firstInstance=1;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'failed stream upload cannot suppress singletons' {$f=Fixture;$f.counters.shadowBatchExecution.uploadSucceeded=$false;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'unsafe frame or snapshot slot rejected' {$f=Fixture;$f.counters.shadowBatchExecution.frameSlot=3;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'forced-refresh cannot claim reuse' {$f=Fixture;$f.frame.shadow.reuseRegionCount.value=1;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'Singleton request cannot report grouped execution' {$f=Fixture;$f.frame.effectiveOptions.shadowSubmission='Singleton';VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'selected page pieces exactly partition source span' {$f=Fixture;$f.counters.eligibleCasterIdentityRows[1].objectId=$f.counters.eligibleCasterIdentityRows[0].objectId;$f.counters.eligibleCasterIdentityRows[0].meshletCount=2;$f.counters.eligibleCasterIdentityRows[1].firstMeshlet=2;$f.counters.eligibleCasterIdentityRows[1].meshletCount=2;$f.counters.submittedCasterIdentityRows=Clone $f.counters.eligibleCasterIdentityRows;$f.counters.shadowBatchExecution.sources=@($f.counters.shadowBatchExecution.sources[0]);$f.counters.shadowBatchExecution.groups[0].members[1].objectId=$f.counters.shadowBatchExecution.groups[0].members[0].objectId;VerifyNativeShadowBatchExecution $f.frame $f.counters}
Case 'complete upload-failure singleton fallback is valid' {$f=Fixture $true;$f.frame.effectiveOptions.shadowSubmission='Grouped';VerifyNativeShadowBatchExecution $f.frame $f.counters}
Case 'submitted duplicate cannot hide a missing executed member' {$f=Fixture;$f.counters.submittedCasterIdentityRows[1]=Clone $f.counters.submittedCasterIdentityRows[0];VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'numeric uint64 native identity is not a decimal string' {$f=Fixture;$f.counters.shadowBatchExecution.backendGeneration=23;VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'noncanonical uint64 leading zero is rejected' {$f=Fixture;$f.counters.shadowBatchExecution.allocationIdentity='021';VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
Case 'uint64 identity overflow rejected' {$f=Fixture;$f.counters.shadowBatchExecution.allocationGeneration='18446744073709551616';VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
function Reduction([int]$Draws,[int]$Eligible=1000){return [pscustomobject]@{shadow=[pscustomobject]@{actualDrawSubmissions=(W $Draws);eligibleCasterSegments=(W $Eligible)}}}
Case 'exact 90 percent actual reduction passes' {VerifyNativeShadowBatchReduction (Reduction 100) (Reduction 1000)}
Case '89.9 percent actual reduction fails' {VerifyNativeShadowBatchReduction (Reduction 101) (Reduction 1000)} $true
Case 'empty candidate is not an optimized submission proof' {VerifyNativeShadowBatchReduction (Reduction 0) (Reduction 1000)} $true
Case 'unbatched reference requires every eligible singleton' {VerifyNativeShadowBatchReduction (Reduction 99) (Reduction 999)} $true
Case 'different region eligible membership cannot aggregate' {VerifyNativeShadowBatchReduction (Reduction 90 1000) (Reduction 1000 1001)} $true
# Region/cohort tests use exact small native key receipts, not declared counts.
function Regions(){
	$result=@();foreach($view in @('Scene','Game')){foreach($light in @(@('40',1,6),@('41',0,1),@('42',2,1),@('43',3,1))){for($face=0;$face -lt $light[2];$face++){
		$result+=,[pscustomobject]@{operationId=('a'*32);fixtureStepId='shadow-batching-04';measurementPhase='Measured';stepFrame='60';effectiveOptions=[pscustomobject]@{shadowReuse='AlwaysRefresh';shadowSubmission='Grouped'};qualifiedShadowViewKey=[pscustomobject]@{viewType=$view;viewId=$(if($view -ceq 'Scene'){1}else{2});lightId=('650000000000000000000000000000'+$light[0]);resourceKind=$light[1];faceIndex=$face;lightProjectionStamp=[pscustomobject]@{cubeProjectionEvidenceComplete=$true;cubeViewProjections=@((@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),(@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),(@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),(@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),(@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)),(@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)))}};readbackProvenance=[pscustomobject]@{frameSerial='10';commandSerial=$(if($view -ceq 'Scene'){'11'}else{'12'});submitSerial='13';completionSerial='13';settingsGeneration='2';mutationGeneration='3';rendererSessionGeneration='4';fixtureControlGeneration='5';optionsSessionGeneration='6'}}
	}}};return $result
}
Case '18 qualified native regions allow separate Scene Game commands' {VerifyNativeShadowBatchRegions (Regions) shadow-batching-04}
Case 'missing 2D light cannot hide behind complete point cubes' {$r=@(Regions|Where-Object {$_.qualifiedShadowViewKey.resourceKind -ne 3});VerifyNativeShadowBatchRegions $r shadow-batching-04} $true
Case 'extra undeclared view is rejected' {$r=Regions;$r[0].qualifiedShadowViewKey.viewId=9;VerifyNativeShadowBatchRegions $r shadow-batching-04} $true
Case 'coherent regions cannot use mixed primary commands within one view' {$r=Regions;$r[1].readbackProvenance.commandSerial='99';VerifyNativeShadowBatchRegions $r shadow-batching-04} $true
Case 'all regions must observe one control generation' {$r=Regions;$r[1].readbackProvenance.fixtureControlGeneration='99';VerifyNativeShadowBatchRegions $r shadow-batching-04} $true
Case 'all regions must observe one options epoch' {$r=Regions;$r[1].readbackProvenance.optionsSessionGeneration='99';VerifyNativeShadowBatchRegions $r shadow-batching-04} $true
Case 'mixed region effective options cannot pass' {$r=Regions;$r[1].effectiveOptions.shadowSubmission='Singleton';VerifyNativeShadowBatchRegions $r shadow-batching-04} $true
function RecipeFixture(){
	$f=Fixture;$f.frame|Add-Member measurementPhase Transition;$f.frame.shadow|Add-Member fallbackReason (W 'ShadowInstanceUploadUnavailable');$rows=@();for($i=1;$i -le 1000;$i++){$rows+=,(Row $i)};$f.counters.eligibleCasterIdentityRows=$rows;$f.counters.submittedCasterIdentityRows=Clone $rows;$f.counters|Add-Member nativeShadowDiagnostics ([pscustomobject]@{fallbackReason='ShadowInstanceUploadUnavailable'});return $f
}
Case 'sealed recipe requires actual 1000 logical caster owners' {$f=RecipeFixture;VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-00';action='compatible-1000'})}
Case '999 logical source owners cannot claim 1000 fixture' {$f=RecipeFixture;$f.counters.eligibleCasterIdentityRows=@($f.counters.eligibleCasterIdentityRows|Select-Object -First 999);VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-00';action='compatible-1000'})} $true
Case 'wrong sealed action is rejected' {$f=RecipeFixture;VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-00';action='cutout'})} $true
Case 'one planned group does not prove actual dispatch splitting' {$f=RecipeFixture;VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-05';action='dispatch-limit-split'})} $true
Case 'singleton reference need not invent split groups' {$f=RecipeFixture;$f.frame.effectiveOptions.shadowSubmission='Singleton';VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-05';action='dispatch-limit-split'})}
Case 'first injected upload failure requires actual singleton fallback' {$f=RecipeFixture;VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-06';action='upload-failure-fallback'})} $true
Case 'first failed upload complete singleton fallback is valid' {$f=RecipeFixture;$f.counters.shadowBatchExecution.groups=@();$f.counters.shadowBatchExecution.singletons=@($f.counters.submittedCasterIdentityRows|ForEach-Object {[pscustomobject]@{objectId=$_.objectId;lifetimeGeneration=$_.lifetimeGeneration;segmentId=$_.segmentId}});$f.counters.shadowBatchExecution.uploadSucceeded=$false;VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-06';action='upload-failure-fallback'})}
Case 'later one-shot upload samples may group successfully' {$f=RecipeFixture;$f.frame.measurementPhase='Measured';VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-06';action='upload-failure-fallback'})}
Case 'old or guessed upload failure reason is rejected' {$f=RecipeFixture;$f.counters.shadowBatchExecution.groups=@();$f.counters.shadowBatchExecution.singletons=@($f.counters.submittedCasterIdentityRows);$f.counters.shadowBatchExecution.uploadSucceeded=$false;$f.counters.nativeShadowDiagnostics.fallbackReason='OldFailure';VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-06';action='upload-failure-fallback'})} $true
Case 'all Scene-visible sources cannot prove off-camera fixture' {$f=RecipeFixture;$f.frame|Add-Member viewType Scene;VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-04';action='off-camera'})} $true
Case 'actual nonvisible Scene source proves off-camera membership' {$f=RecipeFixture;$f.frame|Add-Member viewType Scene;$f.counters.shadowBatchExecution.sources[0].cameraVisible=$false;VerifyNativeShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{id='shadow-batching-04';action='off-camera'})}
$script:definitionRoot=Get-Content -LiteralPath (Join-Path (Split-Path -Parent $Comparer) 'Schemas/evidence.schema.json') -Raw|ConvertFrom-Json
Case 'schema defines exact native execution receipt ABI' {$f=Fixture;Validate $f.counters.shadowBatchExecution $script:definitionRoot.'$defs'.nativeShadowBatchExecution}

# Shared fixtures preserve their independent native shadow/light/depth routes.
Case 'P4 shared lighting decoder is admitted without changing P2 options' {VerifyNativeLightingStage ([pscustomobject]@{stage='P4';fixtureId='recovery-capacity'})}
Case 'dedicated P4 does not acquire an unrelated lighting decoder' {VerifyNativeLightingStage ([pscustomobject]@{stage='P4';fixtureId='shadow-batching'})} $true
function SharedRecipeFixture(){
	$f=Fixture $true;$f.frame.effectiveOptions.shadowSubmission='Grouped';$f.counters.eligibleCasterIdentityRows=@((Row 21),(Row 31));$f.counters.submittedCasterIdentityRows=Clone $f.counters.eligibleCasterIdentityRows
	$f.counters.eligibleCasterIdentityRows[0]|Add-Member deformationPalette @(@(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1));$f.counters.eligibleCasterIdentityRows[1]|Add-Member morphWeights @(.5)
	return $f
}
Case 'actual CPU pose and morph singleton route is complete' {$f=SharedRecipeFixture;VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='unsupported-grouped-route'}) recovery-capacity}
Case 'unsupported route cannot report an actual group' {$f=SharedRecipeFixture;$f.counters.shadowBatchExecution.groups=@((Fixture).counters.shadowBatchExecution.groups[0]);VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='unsupported-grouped-route'}) recovery-capacity} $true
Case 'unsupported route cannot substitute ordinary static owners' {$f=SharedRecipeFixture;$f.counters.eligibleCasterIdentityRows[0].objectId=(Member 1).objectId;VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='unsupported-grouped-route'}) recovery-capacity} $true
Case 'unsupported pose requires actual retained palette input' {$f=SharedRecipeFixture;$f.counters.eligibleCasterIdentityRows[0].deformationPalette=@();VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='unsupported-grouped-route'}) recovery-capacity} $true
Case 'unsupported morph requires actual active weight input' {$f=SharedRecipeFixture;$f.counters.eligibleCasterIdentityRows[1].morphWeights=@(0);VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='unsupported-grouped-route'}) recovery-capacity} $true
function SharedOverflowFixture(){
	$f=RecipeFixture
	for($i=0;$i -lt 1000;$i++){$f.counters.eligibleCasterIdentityRows[$i].objectId='650'+('0'*24)+[string](20000+$i)}
	$f.counters.submittedCasterIdentityRows=Clone $f.counters.eligibleCasterIdentityRows
	return $f
}
Case 'group overflow requires actual compatible pool membership' {$f=SharedOverflowFixture;$f.counters.eligibleCasterIdentityRows=@($f.counters.eligibleCasterIdentityRows|Select-Object -First 999);VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='group-overflow'}) recovery-capacity} $true
Case 'group overflow requires actual split commands' {$f=SharedOverflowFixture;VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='group-overflow'}) recovery-capacity} $true
Case 'shared split rejects a group above actual control64' {$f=SharedOverflowFixture;$f.counters.shadowBatchExecution.groups=@([pscustomobject]@{instanceCount=65},[pscustomobject]@{instanceCount=64});VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='group-overflow'}) recovery-capacity} $true
Case 'exact shared pool permits actual native groups within64' {
	$f=SharedOverflowFixture;$f.counters.shadowBatchExecution.groups=@([pscustomobject]@{instanceCount=64},[pscustomobject]@{instanceCount=64})
	# Same count is insufficient: a wrong authored owner must not substitute for
	# the frozen pool. Catch only the real production rejection, then verify the
	# valid multisegment pool owner set without requiring one row per object.
	$bad=Clone $f;$bad.counters.eligibleCasterIdentityRows[0].objectId=(Member 1).objectId;$rejected=$false
	try{VerifyNativeSharedShadowBatchRecipe $bad.frame $bad.counters ([pscustomobject]@{action='group-overflow'}) recovery-capacity}catch{if($_.Exception.Message.StartsWith('NATIVE_FAILED: ')){$rejected=$true}else{throw}}
	NativeRequire $rejected 'Same-sized wrong pool was admitted'
	$piece=Clone $f.counters.eligibleCasterIdentityRows[0];$piece.segmentId='1001';$f.counters.eligibleCasterIdentityRows+=,$piece
	VerifyNativeSharedShadowBatchRecipe $f.frame $f.counters ([pscustomobject]@{action='group-overflow'}) recovery-capacity
}
Case 'shared baseline actual5 uncapped lights is preserved' {$proof=@{counters=[pscustomobject]@{stage='P4'};frame=[pscustomobject]@{fullSourceCandidateCount=(W 5)};upload=[pscustomobject]@{activeRecordCount=(W 5)}};VerifyNativeLightingSourceCount $proof ([pscustomobject]@{action='device-recovery'}) recovery-capacity}
Case 'light pressure cannot relabel payloadcap1024 as source1025' {$proof=@{counters=[pscustomobject]@{stage='P4'};frame=[pscustomobject]@{fullSourceCandidateCount=(W 1024)};upload=[pscustomobject]@{activeRecordCount=(W 1024)}};VerifyNativeLightingSourceCount $proof ([pscustomobject]@{action='buffer-overflow'}) recovery-capacity} $true

function SharedCohortFixture([bool]$TwoViews=$false){
	$rows=@();$counters=if($TwoViews){@(Regions)}else{@(Regions|Where-Object {$_.qualifiedShadowViewKey.viewType -ceq 'Scene'})}
	foreach($c in $counters){$c.fixtureStepId='recovery-capacity-05';if($c.qualifiedShadowViewKey.resourceKind -eq 1){$c.qualifiedShadowViewKey.lightId='65000000000000000000000000000002'}
		$f=[pscustomobject]@{fixtureStepId=$c.fixtureStepId;measurementPhase=$c.measurementPhase;stepFrame=$c.stepFrame;viewType=$c.qualifiedShadowViewKey.viewType;viewId=$c.qualifiedShadowViewKey.viewId;evidenceKind='Shadow';effectiveOptions=$c.effectiveOptions};$rows+=,[pscustomobject]@{frame=$f;counters=$c;imageHash=('a'*64)}
	}
	foreach($view in $(if($TwoViews){@('Scene','Game')}else{@('Scene')})){
		$first=@($rows|Where-Object {$_.frame.viewType -ceq $view})[0]
		foreach($purpose in @('FinalLights','Analysis')){$f=Clone $first.frame;$f.evidenceKind='Lighting';$f|Add-Member payloadPurpose $purpose;$c=Clone $first.counters;$rows+=,[pscustomobject]@{frame=$f;counters=$c;imageHash=('a'*64)}}
	};return $rows
}
$cameras=@([pscustomobject]@{viewType='Scene';viewId=1});$bothCameras=@($cameras)+@([pscustomobject]@{viewType='Game';viewId=2})
Case 'shared nine Shadow plus both Lighting purposes is coherent' {VerifyNativeP4SharedCohorts (SharedCohortFixture) $cameras recovery-capacity;$one=Clone @(Regions|Where-Object {$_.qualifiedShadowViewKey.viewType -ceq 'Scene' -and $_.qualifiedShadowViewKey.resourceKind -eq 0})[0];$one.fixtureStepId='shadow-batching-00';VerifyNativeShadowBatchRegions @($one) shadow-batching-00}
Case 'shared two-view22 rows permit actual distinct commands' {VerifyNativeP4SharedCohorts (SharedCohortFixture $true) $bothCameras multiview-lights}
Case 'shared missing Analysis payload cannot pass shadow-only correctness' {$rows=@(SharedCohortFixture|Where-Object {!((Has $_.frame 'payloadPurpose') -and $_.frame.payloadPurpose -ceq 'Analysis')});VerifyNativeP4SharedCohorts $rows $cameras recovery-capacity} $true
Case 'shared Lighting cannot come from another native frame' {$rows=SharedCohortFixture;$rows[-1].counters.readbackProvenance.frameSerial='99';VerifyNativeP4SharedCohorts $rows $cameras recovery-capacity} $true
Case 'shared Lighting cannot come from another actual primary' {$rows=SharedCohortFixture;$rows[-1].counters.readbackProvenance.commandSerial='99';VerifyNativeP4SharedCohorts $rows $cameras recovery-capacity} $true
Case 'shared Lighting cannot come from an old control generation' {$rows=SharedCohortFixture;$rows[-1].counters.readbackProvenance.fixtureControlGeneration='99';VerifyNativeP4SharedCohorts $rows $cameras recovery-capacity} $true
Case 'shared Lighting cannot use unrelated rendered-color bytes' {$rows=SharedCohortFixture;$rows[-1].imageHash=('b'*64);VerifyNativeP4SharedCohorts $rows $cameras recovery-capacity} $true
Case 'shared complete point cube cannot hide missing Area region' {$rows=@(SharedCohortFixture|Where-Object {$_.frame.evidenceKind -ceq 'Lighting' -or $_.counters.qualifiedShadowViewKey.resourceKind -ne 3});VerifyNativeP4SharedCohorts $rows $cameras recovery-capacity} $true
Case 'shared1025 light pressure retains actual cap1024 payload' {$proof=@{counters=[pscustomobject]@{stage='P4'};frame=[pscustomobject]@{fullSourceCandidateCount=(W 1025)};upload=[pscustomobject]@{activeRecordCount=(W 1024)}};VerifyNativeLightingSourceCount $proof ([pscustomobject]@{action='buffer-overflow'}) recovery-capacity}
Case 'raw uint32 lane cannot masquerade as a decimal string' {$f=Fixture;$f.counters.shadowBatchExecution.frameSlot='0';VerifyNativeShadowBatchExecution $f.frame $f.counters} $true
$failed=@($results|Where-Object {!$_.passed}).Count
[pscustomobject]@{schemaVersion=1;scope='Synthetic CPU contract tests only';caseCount=$results.Count;failures=$failed;gpuAcceptance=$false;legacyReplay=[bool]$ReplayLegacyShadowBatchAdmission;results=$results}|ConvertTo-Json -Depth 30|Set-Content (Join-Path $root 'result.json')
Write-Output "Renderer efficiency shadow batching evidence contracts completed: $($results.Count) cases, $failed failures."
if($failed){exit 1};exit 0
