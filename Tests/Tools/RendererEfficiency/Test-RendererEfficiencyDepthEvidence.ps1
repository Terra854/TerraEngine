<#
.SYNOPSIS
Exercises isolated native depth evidence guards using small synthetic CPU files.
.DESCRIPTION
Use a fresh WorkRoot. Helpers are AST-extracted without launching the comparer,
editor or GPU. ReplayLegacyTimingAdmission reproduces the old aggregate-only
Metric admission for attributable behavioral RED; it is never GPU acceptance.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WorkRoot,[string]$Comparer='', [switch]$ReplayLegacyTimingAdmission)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
if(!$Comparer){$Comparer=Join-Path $PSScriptRoot 'Compare-RendererEfficiencyEvidence.ps1'}
$root=[IO.Path]::GetFullPath($WorkRoot);if(Test-Path -LiteralPath $root){throw 'Depth contracts require a fresh WorkRoot'}
[IO.Directory]::CreateDirectory($root)|Out-Null
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($Comparer,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Comparer syntax error'}
foreach($node in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){Invoke-Expression $node.Extent.Text}
InitializeNativeProof;InitializeNativeDepthProof
if($ReplayLegacyTimingAdmission){
	# This is the exact former main-loop policy: read the reported frame metric
	# without examining raw native query availability, owner, scope or markers.
	function VerifyNativeDepthQuery($Query,$Frame,$Provenance,[string]$Purpose,[bool]$Nonempty){return Metric $Frame.depthStageTimingNanoseconds depthStageTimingNanoseconds}
}
$utf8=[Text.UTF8Encoding]::new($false)
function W($value){return [pscustomobject]@{status='Valid';value=$value;reason=''}}
function U(){return [pscustomobject]@{status='Unavailable';value=$null;reason='No shadow region'}}
function Clone($value){return ConvertTo-Json -InputObject $value -Depth 100 -Compress|ConvertFrom-Json}
$options=[pscustomobject]@{lightingDepth='Cooperative';lightingUploads='Dirty';shadowReuse='Enabled';shadowSubmission='Singleton';staticShadowCacheBudgetBytes=0;validationPreparation='Serial'}
$frame=[pscustomobject]@{evidenceKind='LightingDepth';payloadPurpose='Production';fixtureStepId='depth-coverage-05';stepFrame=60;measurementPhase='Measured';viewType='Scene';viewId=1;publicationState='Complete';effectiveOptions=$options;depthStageTimingNanoseconds=(W 100);lightId=(U);faceIndex=(U);shadow=[pscustomobject]@{actualDrawSubmissions=(U)};artifacts=@()}
$p=[pscustomobject]@{kind='FullNativeSameCommandCompletedReadback';frameSerial='5';commandSerial='6';submitSerial='7';completionSerial='7';settingsGeneration='2';mutationGeneration='3';rendererSessionGeneration='4';fixtureControlGeneration='8';optionsSessionGeneration='9'}
foreach($field in @('frameSerial','commandSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration')){$frame|Add-Member $field (W ([int]$p.$field))}
function PhysicalUpload($purpose){return [pscustomobject]@{purpose=$purpose;allocationId=(W $(if($purpose -ceq 'FinalLights'){44}else{45}));allocationGeneration=(W 10);allocationSizeBytes=(W 1441792);regionOffsetBytes=(W 0);regionSizeBytes=(W 180224);nonCoherentAtomSize=(W 256);frameSlot=(W 0);snapshotSlot=(W 0);activeRecordCount=(W 1);logicalWriteBytes=(W 0);logicalWriteRecords=(W 0);logicalWriteRanges=(W @());physicalFlushBytes=(W 0);physicalFlushRanges=(W @());mirrorCpuDurationNanoseconds=(W 20);coherent=$true;hostWriteStatus='Succeeded';reason=''}}
$frame|Add-Member lighting @((PhysicalUpload FinalLights),(PhysicalUpload Analysis))
function Query($purpose='Production'){
	return [pscustomobject]@{owner=[pscustomobject]@{rendererSessionGeneration='4';backendResourceGeneration='10';queryPoolGeneration='11';frameSerial='5';commandSerial='6';frameSlot=0;snapshotSlot=0;outputId=1;purpose=$purpose;view='Scene';scope='IsolatedDepth'};range=[pscustomobject]@{firstQuery=$(if($purpose -ceq 'Oracle'){2}else{0});count=2};status='Valid';reason='None';beginTicks='0';endTicks='100';rawTicks='100';submitSerial='7';completionSerial='7';timestampValidBits=64;timestampPeriodNanoseconds=1.0;durationNanoseconds=100.0;beginAvailable=$true;endAvailable=$true;nonemptyWork=$true;cooperative=$true;computeStageMarkers=$true;noWaitRead=$true;queryReadFlags=5;beginStageMask=2048;endStageMask=2048}
}
$results=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[scriptblock]$Body,[bool]$Reject=$false){$caught=$null;try{$null=&$Body}catch{$caught=$_.Exception.Message};$pass=if($Reject){$null -ne $caught -and ($caught.StartsWith('NATIVE_FAILED: ') -or $caught.StartsWith('UNVERIFIED: ') -or $caught.StartsWith('Artifact hash mismatch: '))}else{$null -eq $caught};$results.Add([pscustomobject]@{name=$Name;passed=$pass;error=$caught});Write-Output ((@('FAIL ','PASS ')[$pass])+$Name);if(!$pass -and $caught){Write-Output $caught}}
Case 'zero begin tick is valid' {VerifyNativeDepthQuery (Query) $frame $p Production $true}
Case 'eight-bit timestamp rollover is exact' {$q=Query;$q.timestampValidBits=8;$q.beginTicks='250';$q.endTicks='94';VerifyNativeDepthQuery $q $frame $p Production $true}
Case '64-bit rollover is exact' {$q=Query;$q.beginTicks='18446744073709551606';$q.endTicks='90';VerifyNativeDepthQuery $q $frame $p Production $true}
foreach($queryField in @('beginAvailable','endAvailable','computeStageMarkers','noWaitRead')){Case "missing actual $queryField rejected" {$q=Query;$q.$queryField=$false;VerifyNativeDepthQuery $q $frame $p Production $true} $true}
Case 'WAIT read flags rejected' {$q=Query;$q.queryReadFlags=7;VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'PARTIAL read flags rejected' {$q=Query;$q.queryReadFlags=13;VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'whole-view timing rejected' {$q=Query;$q.owner.scope='WholeView';VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'oracle cannot supply production timing' {VerifyNativeDepthQuery (Query Oracle) $frame $p Production $true} $true
Case 'wrong compute stage rejected' {$q=Query;$q.endStageMask=65536;VerifyNativeDepthQuery $q $frame $p Production $true} $true
foreach($ownerField in @('rendererSessionGeneration','frameSerial','commandSerial')){Case "stale query $ownerField rejected" {$q=Query;$q.owner.$ownerField='99';VerifyNativeDepthQuery $q $frame $p Production $true} $true}
Case 'wrong physical query range rejected' {$q=Query;$q.range.firstQuery=2;VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'wrong snapshot slot rejected' {$q=Query;$q.owner.snapshotSlot=8;VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'zero pool generation rejected' {$q=Query;$q.owner.queryPoolGeneration='0';VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'wrong submit rejected' {$q=Query;$q.submitSerial='8';VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'unavailable timing remains unverified' {$q=Query;$q.status='Unavailable';VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'invalid valid-bit width rejected' {$q=Query;$q.timestampValidBits=0;VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'forged raw ticks rejected' {$q=Query;$q.rawTicks='101';VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'rounded duration cannot replace raw ticks' {$q=Query;$q.timestampPeriodNanoseconds=1.234567;$q.durationNanoseconds=123;VerifyNativeDepthQuery $q $frame $p Production $true} $true
Case 'zero nonempty duration unavailable' {$q=Query;$q.endTicks='0';$q.rawTicks='0';$q.durationNanoseconds=0;$f=Clone $frame;$f.depthStageTimingNanoseconds=W 0;VerifyNativeDepthQuery $q $f $p Production $true} $true
Case 'actual fallback mode cannot claim cooperative query' {$q=Query;$q.cooperative=$false;VerifyNativeDepthQuery $q $frame $p Production $true} $true
# Build expected bytes independently in the test, including empty sub-grid
# rectangles. Native validation reads these supplied bytes afresh each call.
$recipe=[pscustomobject]@{width=7;height=5;fillBits=1065353216;overrides=@()}
for($y=0;$y -lt 5;$y++){for($x=0;$x -lt 7;$x++){$recipe.overrides+=@([pscustomobject]@{x=$x;y=$y;bits=@(1065353216,0,1065353215)[($y*7+$x)%3]})}}
$taskInput=[TerraRendererDepthProof]::Materialize(7,5,[uint32]$recipe.fillBits,[int[]]@($recipe.overrides|ForEach-Object x),[int[]]@($recipe.overrides|ForEach-Object y),[uint32[]]@($recipe.overrides|ForEach-Object bits))
$bounds=[byte[]]::new(1152);$coverage=[byte[]]::new(1152)
for($ty=0;$ty -lt 9;$ty++){for($tx=0;$tx -lt 16;$tx++){$i=$ty*16+$tx;$lo=[single]1;$hi=[single]0;$visited=[uint32]0;$eligible=[uint32]0;for($y=[TerraRendererDepthProof]::Edge($ty,5,9);$y -lt [TerraRendererDepthProof]::Edge($ty+1,5,9);$y++){for($x=[TerraRendererDepthProof]::Edge($tx,7,16);$x -lt [TerraRendererDepthProof]::Edge($tx+1,7,16);$x++){$visited++;$v=[BitConverter]::ToSingle($taskInput,($y*7+$x)*4);if(![single]::IsNaN($v) -and ![single]::IsInfinity($v) -and $v -ge 0 -and $v -lt 1){$eligible++;$lo=[Math]::Min($lo,$v);$hi=[Math]::Max($hi,$v)}}};if(!$eligible){$hi=[single]1};[Array]::Copy([BitConverter]::GetBytes($lo),0,$bounds,$i*8,4);[Array]::Copy([BitConverter]::GetBytes($hi),0,$bounds,$i*8+4,4);[Array]::Copy([BitConverter]::GetBytes($visited),0,$coverage,$i*8,4);[Array]::Copy([BitConverter]::GetBytes($eligible),0,$coverage,$i*8+4,4)}}
function OracleCheck($b=$bounds,$c=$coverage,$bytes=$taskInput){try{[TerraRendererDepthProof]::Oracle(7,5,$bytes,$b,$c)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}}
Case 'sub-grid actual 35 texels and empty rectangles accepted' {OracleCheck}
Case 'missing GPU visited texel rejected' {$c=$coverage.Clone();$idx=0;while([BitConverter]::ToUInt32($c,$idx*8) -eq 0){$idx++};$c[$idx*8]=0;OracleCheck $bounds $c} $true
Case 'missing GPU eligible texel rejected' {$c=$coverage.Clone();$idx=0;while([BitConverter]::ToUInt32($c,$idx*8+4) -eq 0){$idx++};$c[$idx*8+4]=0;OracleCheck $bounds $c} $true
Case 'near-clear eligible texel cannot disappear within tolerance' {$b=$bounds.Clone();$idx=0;while([BitConverter]::ToSingle($b,$idx*8) -ne [BitConverter]::ToSingle([BitConverter]::GetBytes([uint32]1065353215),0)){$idx++};[Array]::Copy([BitConverter]::GetBytes([single]1),0,$b,$idx*8,4);[Array]::Copy([BitConverter]::GetBytes([single]1),0,$b,$idx*8+4,4);OracleCheck $b} $true
Case 'invalid values are ineligible including NaN Inf negative clear' {foreach($bits in @([uint32]2143289345,[uint32]2139095040,[uint32]4286578688,[uint32]3212836864,[uint32]1065353216,[uint32]1069547520)){NativeRequire (![TerraRendererDepthProof]::Eligible([BitConverter]::ToSingle([BitConverter]::GetBytes($bits),0))) 'Invalid depth accepted'}}
Case 'negative zero is eligible without bit normalization' {NativeRequire ([TerraRendererDepthProof]::Eligible([BitConverter]::ToSingle([BitConverter]::GetBytes([uint32]2147483648),0))) 'Signed zero lost'}
Case 'exact empty class before tolerance' {$empty=[byte[]]::new(1152);for($i=0;$i -lt 288;$i++){[Array]::Copy([BitConverter]::GetBytes([single]1),0,$empty,$i*4,4)};try{[TerraRendererDepthProof]::BoundsDelta($empty,$bounds)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}} $true
Case '299 samples never speed proof' {VerifyNativeDepthSpeed (@(80)*299) (@(100)*300)} $true
Case '300 real admitted samples meet exact 10 percent' {VerifyNativeDepthSpeed (@(90)*300) (@(100)*300)}
Case 'less than ten percent rejected' {VerifyNativeDepthSpeed (@(91)*300) (@(100)*300)} $true
Case 'truthful complete-scan fallback preserves parity only' {$q=Query;$q.cooperative=$false;$f=Clone $frame;$f|Add-Member depthFallbackReason (W 'Cooperative pipeline unavailable');VerifyNativeDepthQuery $q $f $p Production $true}
# These on-disk artifacts use the real production reader and its fresh hash,
# closure, query and color joins. Their contents remain synthetic CPU fixtures.
Add-Type -AssemblyName System.Drawing
$bitmap=[Drawing.Bitmap]::new(7,5);$stream=[IO.MemoryStream]::new()
try{$bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png);$png=$stream.ToArray()}finally{$bitmap.Dispose();$stream.Dispose()}
$identity=[pscustomobject]@{fixtureId='depth-coverage';fixtureHash=(W ('a'*64));runId='depth-cpu';runIndex=0;seed=65004;mode='Candidate';stage='P3';resolution=[pscustomobject]@{width=7;height=5}}
$counter=[pscustomobject]@{schemaVersion=1;evidenceKind='LightingDepth';fixtureId='depth-coverage';fixtureHash=('a'*64);operationId=('4'*32);fixtureStepId=$frame.fixtureStepId;runId='depth-cpu';runIndex=0;seed=65004;mode='Candidate';stage='P3';measurementPhase='Measured';stepFrame='60';viewType='Scene';viewId=1;effectiveOptions=$options;readbackProvenance=$p;nativePreparation=[pscustomobject]@{workerTasks=0;inlineTasks=4;settled=$true};productionNativeExtent=@(7,5);tileCount=144;boundsEncoding='float32-ieee-le';coverageEncoding='uint32-pair-le';productionQuery=(Query);oracleQuery=(Query Oracle);oracleRecipe=$recipe;colorNativeExtent=@(7,5);colorSourceFormat=44;imageEncoding='png-rgba8';depthFallbackReason='';lightingUploads=@([pscustomobject]@{purpose='FinalLights';available=$true;succeeded=$true;allocationGeneration='10';frameSlot=0;snapshotSlot=0},[pscustomobject]@{purpose='Analysis';available=$true;succeeded=$true;allocationGeneration='10';frameSlot=0;snapshotSlot=0})}
$ordinal=0
function SaveDepth($f,$c,$bytes=$taskInput,$gpuBounds=$bounds,$gpuCoverage=$coverage){
	$script:ordinal++;$artifacts=[Collections.Generic.List[object]]::new()
	foreach($part in @(@('ProductionDepthBounds','production.f32',$bounds),@('DepthInput','input.bin',$bytes),@('DepthBounds','oracle.f32',$gpuBounds),@('DepthCoverage','coverage.bin',$gpuCoverage),@('Image','image.png',$png),@('Counters','counters.json',$utf8.GetBytes((Canonical $c))))){$relative="$script:ordinal-$($part[1])";$path=Join-Path $root $relative;[IO.File]::WriteAllBytes($path,[byte[]]$part[2]);$artifacts.Add([pscustomobject]@{kind=$part[0];status='Valid';path=$relative;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();reason=''})}
	$f.artifacts=$artifacts.ToArray();return ReadNativeDepthProof $f $identity $root (NewNativeArtifactClosureIndex $f.artifacts)
}
Case 'full synthetic artifact/query/color proof accepted' {SaveDepth (Clone $frame) (Clone $counter)}
Case 'actual GPU input byte mismatch rejected' {$b=$taskInput.Clone();$b[0]=1;SaveDepth (Clone $frame) (Clone $counter) $b} $true
Case 'short actual oracle input rejected' {SaveDepth (Clone $frame) (Clone $counter) ([byte[]]::new(4))} $true
Case 'same-command color extent mismatch rejected' {$c=Clone $counter;$c.colorNativeExtent=@(8,5);SaveDepth (Clone $frame) $c} $true
Case 'current raw control generation missing rejected' {$c=Clone $counter;$c.readbackProvenance.fixtureControlGeneration='0';SaveDepth (Clone $frame) $c} $true
Case 'wrong raw mutation generation rejected' {$c=Clone $counter;$c.readbackProvenance.mutationGeneration='99';SaveDepth (Clone $frame) $c} $true
Case 'oracle or query from another physical upload rejected' {
	$rejected=0
	foreach($change in @('oracleBackend','productionBackend','frameSlot','snapshotSlot')){
		$c=Clone $counter
		switch($change){
			'oracleBackend'{$c.oracleQuery.owner.backendResourceGeneration='99'}
			'productionBackend'{$c.productionQuery.owner.backendResourceGeneration='99';$c.oracleQuery.owner.backendResourceGeneration='99'}
			'frameSlot'{$c.productionQuery.owner.frameSlot=1;$c.oracleQuery.owner.frameSlot=1;$c.productionQuery.range.firstQuery=32;$c.oracleQuery.range.firstQuery=34}
			'snapshotSlot'{$c.productionQuery.owner.snapshotSlot=1;$c.oracleQuery.owner.snapshotSlot=1;$c.productionQuery.range.firstQuery=4;$c.oracleQuery.range.firstQuery=6}
		}
		try{SaveDepth (Clone $frame) $c}catch{if(!$_.Exception.Message.StartsWith('NATIVE_FAILED: ')){throw};$rejected++}
	}
	NativeRequire ($rejected -eq 4) 'Internally valid query range escaped exact uploaded physical owner'
}
Case 'corrupt copied artifact hash rejected' {$proof=SaveDepth (Clone $frame) (Clone $counter);$path=Join-Path $root $proof.frame.artifacts[0].path;[IO.File]::WriteAllBytes($path,[byte[]]::new(1152));ReadNativeDepthProof $proof.frame $identity $root} $true
Case 'artifact absent root closure rejected' {$proof=SaveDepth (Clone $frame) (Clone $counter);ReadNativeDepthProof $proof.frame $identity $root (NewNativeArtifactClosureIndex @())} $true
Case 'sealed recipe cannot be replaced by matched alternate bits' {$proof=SaveDepth (Clone $frame) (Clone $counter);$r=Clone $recipe;$r.fillBits=0;VerifyNativeDepthRecipe $proof @{$frame.fixtureStepId=@{viewType='Scene';viewId=1;recipe=$r}}} $true
Case 'one depth plus two lighting purposes form exact cohort' {$proof=SaveDepth (Clone $frame) (Clone $counter);$lighting=@();foreach($purpose in @('FinalLights','Analysis')){$f=Clone $frame;$f.evidenceKind='Lighting';$f.payloadPurpose=$purpose;$lighting+=@(@{frame=$f;counters=(Clone $counter);imageHash=$proof.imageHash})};VerifyNativeDepthCohorts @($proof) $lighting @([pscustomobject]@{viewType='Scene';viewId=1});$lighting[0].imageHash='f'*64;$rejected=$false;try{VerifyNativeDepthCohorts @($proof) $lighting @([pscustomobject]@{viewType='Scene';viewId=1})}catch{if(!$_.Exception.Message.StartsWith('NATIVE_FAILED: ')){throw};$rejected=$true};NativeRequire $rejected 'Different same-command color accepted'}
Case 'cohort cannot omit declared second view' {$proof=SaveDepth (Clone $frame) (Clone $counter);VerifyNativeDepthCohorts @($proof) @() @([pscustomobject]@{viewType='Scene';viewId=1},[pscustomobject]@{viewType='Game';viewId=2})} $true
Case 'P3 shared actual32 sources accepted' {$proof=@{frame=[pscustomobject]@{fullSourceCandidateCount=(W 32)};upload=[pscustomobject]@{activeRecordCount=(W 32)};counters=[pscustomobject]@{stage='P3'}};VerifyNativeLightingSourceCount $proof ([pscustomobject]@{action='stable'}) 'multiview-lights'}
Case 'P2 shared counts remain five' {$proof=@{frame=[pscustomobject]@{fullSourceCandidateCount=(W 5)};upload=[pscustomobject]@{activeRecordCount=(W 5)};counters=[pscustomobject]@{stage='P2'}};VerifyNativeLightingSourceCount $proof ([pscustomobject]@{action='stable'}) 'multiview-lights'}
function RecipeAuthority(){
	$directory=Join-Path $root 'renderer-efficiency-depth';[IO.Directory]::CreateDirectory($directory)|Out-Null
	$nativeRecipe=[pscustomobject]@{extent=[pscustomobject]@{width=7;height=5};fillBits=$recipe.fillBits;overrides=$recipe.overrides}
	$file=[pscustomobject]@{schemaVersion=1;kind='LightingDepthInputRecipe';fixtureId='depth-coverage';fixtureHash=('a'*64);seed=65004;fixtureStepId=$frame.fixtureStepId;viewType='Scene';viewId=1;grid=@(16,9,24);recipe=$nativeRecipe}
	$relative="renderer-efficiency-depth/$($frame.fixtureStepId).json";$path=Join-Path $root $relative;[IO.File]::WriteAllText($path,(Canonical $file),$utf8)
	$hash=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();$descriptor=[pscustomobject]@{kind='Counters';status='Valid';path=$relative;sha256=$hash;reason=''}
	return @{envelope=[pscustomobject]@{receipts=@([pscustomobject]@{kind='DepthInputRecipe';fixtureStepId=$frame.fixtureStepId;recipePath=$relative;recipeSha256=$hash;nativeExtent=$nativeRecipe.extent;recipe=$nativeRecipe})};document=[pscustomobject]@{artifacts=@($descriptor)};steps=@([pscustomobject]@{id=$frame.fixtureStepId})}
}
Case 'sealed nested extent recipe normalizes exact raw bits' {$a=RecipeAuthority;$map=ReadNativeDepthRecipes $a.envelope $a.document $identity $root $a.steps;$proof=SaveDepth (Clone $frame) (Clone $counter);VerifyNativeDepthRecipe $proof $map}
Case 'authority receipt cannot rewrite sealed recipe bits' {$a=RecipeAuthority;$a.envelope.receipts[0].recipe=Clone $a.envelope.receipts[0].recipe;$a.envelope.receipts[0].recipe.fillBits=0;ReadNativeDepthRecipes $a.envelope $a.document $identity $root $a.steps} $true
Case 'authority source outside root closure rejected' {$a=RecipeAuthority;$a.document.artifacts=@();ReadNativeDepthRecipes $a.envelope $a.document $identity $root $a.steps} $true
Case 'missing recipe authority remains unverified' {$a=RecipeAuthority;$a.envelope.receipts=@();ReadNativeDepthRecipes $a.envelope $a.document $identity $root $a.steps} $true
Case 'actual depth frame and artifact kinds satisfy schema' {
	$script:definitionRoot=Get-Content -LiteralPath (Join-Path (Split-Path $Comparer) 'Schemas/evidence.schema.json') -Raw|ConvertFrom-Json
	$f=Clone $frame;$f|Add-Member totalFrameTimingNanoseconds (U);$f|Add-Member depthFallbackReason (U)
	foreach($field in $definitionRoot.'$defs'.shadow.required){if(!(Has $f.shadow $field)){$f.shadow|Add-Member $field (U)}}
	$f|Add-Member memory ([pscustomobject]@{})
	foreach($field in $definitionRoot.'$defs'.memory.required){$f.memory|Add-Member $field $(if($field -ceq 'draining'){'Unavailable'}else{U})}
	$proof=SaveDepth $f (Clone $counter);Validate $proof.frame $definitionRoot.'$defs'.frame
}
$failed=@($results|Where-Object {!$_.passed}).Count
[IO.File]::WriteAllText((Join-Path $root 'result.json'),(Canonical ([ordered]@{schemaVersion=1;scope='Synthetic CPU depth contracts only';gpuAcceptance=$false;legacyTimingReplay=[bool]$ReplayLegacyTimingAdmission;cases=$results.Count;failed=$failed;results=$results.ToArray()})),$utf8)
Write-Output "Renderer efficiency depth evidence contracts completed: $($results.Count) cases, $failed failures."
if($failed){exit 1};exit 0
