<#
Installed custom-effect integration collector. Requires PowerShell 7, an installed
BUILD_TESTING tree, its bundled SDK/compiler, and an unlocked Windows Vulkan
desktop. Run the INSTALLED script; see AuthoringFixture.md for the native/managed
observer contract. This runner launches only its own disposable project/processes.
-DryRun checks source/input contracts and writes status=dry-run, never passed.
Missing importer, fixture, runtime capture integration, pixels, or acknowledgements
fail a real run. Shader text is copied from dedicated bundled HLSL, never embedded.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$OutputDirectory,
	[ValidateRange(1024,65535)][int]$McpPort=43237,
	[ValidateRange(60,1800)][int]$TimeoutSeconds=900,
	[switch]$DryRun
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Require([bool]$Condition,[string]$Message) { if(!$Condition){throw $Message} }
function Below([string]$Child,[string]$Parent) { return $Child.StartsWith($Parent.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) }
function WriteJson([string]$Path,$Value) {
	$temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
	[IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 100),[Text.UTF8Encoding]::new($false))
	[IO.File]::Move($temporary,$Path,$true)
}
function ReadJson([string]$Path) {
	$stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite-bor[IO.FileShare]::Delete))
	$reader=[IO.StreamReader]::new($stream)
	try{return $reader.ReadToEnd()|ConvertFrom-Json -AsHashtable}finally{$reader.Dispose()}
}
function Pin([string]$Path) {
	Require (Test-Path -LiteralPath $Path -PathType Leaf) "Missing actual artifact: $Path"
	return @{path=[IO.Path]::GetFullPath($Path);length=(Get-Item -LiteralPath $Path).Length;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$install=[IO.Path]::GetFullPath($InstallRoot);$work=[IO.Path]::GetFullPath($OutputDirectory)
Require (!(Test-Path -LiteralPath $work)) 'OutputDirectory must be fresh.'
Require ($work-ine$install-and!(Below $work $install)-and!(Below $install $work)) 'Evidence and install trees must be disjoint.'
$null=[IO.Directory]::CreateDirectory($work)
$runId='authoring-'+[Guid]::NewGuid().ToString('N')
$editor=Join-Path $install 'Editor/TerraEngine-Editor.exe';$exporter=Join-Path $install 'Editor/TerraEngine-Export.exe'
$fixture=Join-Path $install 'Tests/Fixtures/PostProcessing'
$shader=Join-Path $install 'Editor/Shaders/PostProcess/Validation/PostProcessFixtureTint.frag.hlsl'
$materializer=Join-Path $install 'Tests/Tools/PostProcessing/TestInstalledPostProcessColor.ps1'
$cameraIds=@{CameraA='75000000000000000000000000000001';CameraB='75000000000000000000000000000002'}
$shaderId='75000000000000000000000000000010';$effectId='75000000000000000000000000000011';$profileId='75000000000000000000000000000012'
$missingId='750000000000000000000000000000ff';$width=1280;$height=720
$tags=@('baseline','enabled','parameter-changed','disabled')
$checks=@('custom-toggle-on','custom-parameter-change','custom-toggle-off','custom-camera-isolation','authored-profile-binding')
$failedImportRelative=''
$summary=[ordered]@{schemaVersion=1;runId=$runId;status='running';failure='';qualificationComplete=$false
	scope='installed-custom-effect-import-play-isolated-export';installRoot=$install;prerequisites=@();inputs=@();lanes=@();comparisons=@()
	profileVolumeTraversalVerified=$false;missingPackagedPayloadVerified=$false}
$required=@($editor,$exporter,$materializer,$shader,(Join-Path $fixture 'Scripts/CustomPostProcessFixture.cs'),(Join-Path $fixture 'Assembly-CSharp.csproj'))
function NewEffect {
	return @{assetType='TerraEngine.PostProcessEffect';schemaVersion=1;assetId=$effectId;shaderAssetId=$shaderId;revision='1';stableId='fixture-tint';entryPoint='PSMain'
		stage='display-image';domain='display-linear';outputAccess='distinct-color-write';inputs=@(@{kind='source-color';access='sampled-read';binding=0;combinedSampler=$true;assetId=('0'*32)})
		samplerBinding=$null;parameterBytes=20;parameters=@(@{name='tint';kind='vector4';offsetBytes=0;default=@(1,1,1,1);minimum=@(0,0,0,0);maximum=@(2,2,2,2)},@{name='gain';kind='scalar';offsetBytes=16;default=1;minimum=0;maximum=2})
		after=@();assetDependencies=@();requiredCapabilities='0'}
}
function ShaderImportSettings {
	# Dedicated fullscreen shaders have their own bounded push-constant ABI.
	# The ordinary surface importer keeps its engine push-constant restriction.
	return @{stage='ps';entryPoint='PSMain';target='ps_6_0';shaderUse='post-process'}
}
function ResolveImportedArtifact([string]$ProjectRoot,[string]$CompiledPath) {
	Require (![string]::IsNullOrWhiteSpace($CompiledPath)-and![IO.Path]::IsPathRooted($CompiledPath)) 'Importer compiledPath must be a nonempty relative metadata reference.'
	# AssetDatabase owns Build/Assets as its buildRoot. compiledPath is relative
	# to that root, not to Build or the source asset directory. Never try several
	# plausible roots and accept whichever stale artifact happens to exist.
	$buildAssets=[IO.Path]::GetFullPath((Join-Path $ProjectRoot 'Build/Assets'))
	$compiled=[IO.Path]::GetFullPath((Join-Path $buildAssets $CompiledPath))
	Require (Below $compiled $buildAssets) 'Importer compiledPath escaped the canonical Build/Assets root.'
	return $compiled
}
function ReadImportDiagnostics([string]$Root,[string]$Relative) {
	$messages=[Collections.Generic.List[string]]::new();$leaf=[IO.Path]::GetFileName($Relative)
	foreach($name in @('stdout.log','stderr.log')){
		$path=Join-Path $Root $name
		if(!(Test-Path -LiteralPath $path -PathType Leaf)){continue}
		# The owned process can still be draining diagnostics. Read a shared
		# snapshot now; the outer failure handler rereads after StopOwned flushes.
		$stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite-bor[IO.FileShare]::Delete))
		$reader=[IO.StreamReader]::new($stream)
		try{$lines=$reader.ReadToEnd()-split"`r?`n"}finally{$reader.Dispose()}
		foreach($line in $lines){if($line-match([regex]::Escape($leaf))-or$line-match'(?i)(Shader compiler:|dxc failed|Asset compilation failed|Authored surface shaders|Asset import did not complete)'){$messages.Add(($name+': '+$line))}}
	}
	return @($messages.ToArray()|Select-Object -Last 20)
}
function StartOwned([string]$Executable,[string[]]$Arguments,[string]$Root,[hashtable]$Environment) {
	$null=[IO.Directory]::CreateDirectory($Root)
	$start=[Diagnostics.ProcessStartInfo]::new($Executable);$start.WorkingDirectory=Split-Path -Parent $Executable
	$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
	$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
	foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
	foreach($key in $Environment.Keys){$start.Environment[$key]=[string]$Environment[$key]}
	$process=[Diagnostics.Process]::Start($start)
	# Read sharing permits actionable compiler errors while the child is alive.
	# StopOwned still drains and closes these exact owned streams before final
	# failure evidence is read, so buffered tail diagnostics are also retained.
	$share=[IO.FileShare]::ReadWrite-bor[IO.FileShare]::Delete
	$out=[IO.FileStream]::new((Join-Path $Root 'stdout.log'),[IO.FileMode]::Create,[IO.FileAccess]::Write,$share)
	$err=[IO.FileStream]::new((Join-Path $Root 'stderr.log'),[IO.FileMode]::Create,[IO.FileAccess]::Write,$share)
	return @{process=$process;stdout=$out;stderr=$err;outTask=$process.StandardOutput.BaseStream.CopyToAsync($out);errTask=$process.StandardError.BaseStream.CopyToAsync($err)}
}
function StopOwned($Owner) {
	$result=@{forcedTermination=$false;exitCode=-1;cleanupFailure=''}
	try{
		if(!$Owner.process.HasExited){$null=$Owner.process.CloseMainWindow();if(!$Owner.process.WaitForExit(10000)){$result.forcedTermination=$true;$Owner.process.Kill($true);$Owner.process.WaitForExit()}}
		$result.exitCode=$Owner.process.ExitCode;$null=$Owner.outTask.GetAwaiter().GetResult();$null=$Owner.errTask.GetAwaiter().GetResult()
	}catch{$result.cleanupFailure=$_.Exception.Message}finally{foreach($resource in @($Owner.stdout,$Owner.stderr,$Owner.process)){try{$resource.Dispose()}catch{$result.cleanupFailure+='; '+$_.Exception.Message}}}
	return $result
}
$rpcId=0
function Rpc([string]$Method,$Parameters) {
	$request=@{jsonrpc='2.0';id=(++$script:rpcId);method=$Method;params=$Parameters}
	$timeout=if($Method-ceq'tools/call'-and$Parameters.name-ceq'camera.capture_object'){45}elseif($Method-ceq'tools/call'-and$Parameters.name-ceq'asset.import'){180}else{10}
	$response=Invoke-RestMethod -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request|ConvertTo-Json -Depth 40 -Compress) -TimeoutSec $timeout
	# Capture PNGs are retained once, separately from the metadata transcript.
	$logged=$response|ConvertTo-Json -Depth 100|ConvertFrom-Json -AsHashtable
	if($logged.ContainsKey('result')-and$logged.result.ContainsKey('content')){foreach($block in $logged.result.content){if($block.type-ceq'image'){$block.data='<PNG retained in lane evidence>'}}}
	[IO.File]::AppendAllText((Join-Path $work 'rpc.jsonl'),((@{request=$request;response=$logged}|ConvertTo-Json -Depth 100 -Compress)+"`n"))
	Require ($null-eq$response.PSObject.Properties['error']) "Owned editor RPC failed: $Method"
	return $response
}
function Tool([string]$Name,$Arguments=@{},[switch]$AllowFailure) {
	$response=Rpc 'tools/call' @{name=$Name;arguments=$Arguments};$text=@($response.result.content|Where-Object type -eq 'text')
	Require ($text.Count-eq1) "Tool $Name omitted its unique native envelope."
	$envelope=$text[0].text|ConvertFrom-Json -AsHashtable
	if($AllowFailure){return $envelope};Require ($envelope.ok-eq$true) "Native tool $Name failed: $($text[0].text)";return $envelope.data
}
function AwaitPlay([string]$Expected) {
	$clock=[Diagnostics.Stopwatch]::StartNew()
	do{$state=Tool 'play.state';if($state.currentState-ceq$Expected-and$null-eq$state.pendingTransition){return};Start-Sleep -Milliseconds 100}while($clock.Elapsed.TotalSeconds-lt90)
	throw "Owned editor never reached Play state $Expected."
}
function AwaitImports {
	$clock=[Diagnostics.Stopwatch]::StartNew()
	do{$state=Tool 'project.state';if(!$state.assetImportsActive){return};Start-Sleep -Milliseconds 100}while($clock.Elapsed.TotalSeconds-lt$TimeoutSeconds)
	throw 'Actual importer work did not settle before its deadline.'
}
function ImportActual([string]$Relative,[string]$ExpectedImporter) {
	$source=Join-Path $project ('Assets/'+$Relative);$before=Pin $source
	$native=Tool 'asset.import' @{path=$Relative;force=$true}
	AwaitImports
	WriteJson (Join-Path $editorRoot (($Relative-replace'[/\\.]','_')+'-import.json')) $native
	$meta=ReadJson ($source+'.meta');Require ($meta.importerName-ceq$ExpectedImporter) "Wrong actual importer for $Relative."
	if($ExpectedImporter-ceq'ShaderImporter'){Require ($meta.settings.shaderUse-ceq'post-process'-and$meta.settings.stage-ceq'ps'-and$meta.settings.entryPoint-ceq'PSMain') 'Actual shader metadata lost its dedicated post-process import mode.'}
	Require ((Pin $source).sha256-ceq$before.sha256) "Importer rewrote source $Relative."
	$compiled=ResolveImportedArtifact $project ([string]$meta.compiledPath)
	if(!(Test-Path -LiteralPath $compiled -PathType Leaf)){
		$script:failedImportRelative=$Relative;$diagnostics=@(ReadImportDiagnostics $editorRoot $Relative)
		WriteJson (Join-Path $editorRoot (($Relative-replace'[/\\.]','_')+'-unavailable.json')) @{schemaVersion=1;source=$Relative;admission=$native;metadata=$meta;expectedCompiledPath=$compiled;compiledOutputAvailable=$false;compilerDiagnostics=$diagnostics;terminalImportStatusInvented=$false}
		throw ("Actual importer output unavailable at its Build/Assets metadata reference: $compiled. Native admission status '$($native.status)' does not prove compilation. "+($diagnostics-join"`n"))
	}
	$pin=Pin $compiled;$summary.inputs+=,$pin
	WriteJson (Join-Path $editorRoot (($Relative-replace'[/\\.]','_')+'-resolved.json')) @{schemaVersion=1;source=$Relative;admission=$native;metadata=$meta;compiledAssetRoot='Build/Assets';compiledArtifact=$pin;terminalImportStatusInvented=$false}
	return @{metadata=$meta;compiled=$pin;native=$native}
}
function ValidateCheckpoint($Report,[string]$Boot) {
	Require ($Report.schemaVersion-eq1-and$Report.runId-ceq$runId-and$Report.bootId.Length-gt0) 'Authoring checkpoint identity/schema mismatch.'
	if($Boot){Require ($Report.bootId-ceq$Boot) 'Authoring fixture changed boot identity inside one process.'}
	Require ($Report.status-cne'failed') "Managed authoring fixture failed: $($Report.failure)"
	if($Report.phase-cne'Capture'-and$Report.status-cne'passed'){return}
	foreach($name in @('CameraA','CameraB')){
		$snapshot=$Report[$name.Substring(0,1).ToLowerInvariant()+$name.Substring(1)]
		Require ($snapshot.cameraId-ceq$cameraIds[$name]) 'Authoring checkpoint belongs to another camera.'
		foreach($field in @('revision','frameId','sceneGeneration','objectLifetimeGeneration','componentGeneration','bindingGeneration','viewLifetimeGeneration','rendererSessionGeneration','backendResourceGeneration')){Require ([uint64]$snapshot[$field]-gt0) "Missing native authoring snapshot token $field. $name"}
	}
	$effect=$Report.cameraA.customEffect
	Require ($effect.assetId-ceq$effectId-and$effect.shaderAssetId-ceq$shaderId) 'Effective custom state belongs to another asset/shader.'
	if($Report.phase-ceq'Capture'){
		$expectedEnabled=$Report.captureTag-cin@('enabled','parameter-changed');Require ($effect.enabled-eq$expectedEnabled) 'Actual effective custom enable differs from the phase.'
		if($expectedEnabled){$gain=if($Report.captureTag-ceq'enabled'){1.0}else{.5};Require ([double]$effect.gain-eq$gain-and@($effect.tint).Count-eq4) 'Effective typed gain/tint is absent or wrong.';foreach($axis in 0..3){Require ([double]$effect.tint[$axis]-eq@(.5,1,.75,1)[$axis]) 'Effective independent tint differs from the authored patch.'}}
	}
}
function CaptureActual([string]$Root,$Report,[string]$Camera,[string]$Stage) {
	$snapshot=$Report[$Camera.Substring(0,1).ToLowerInvariant()+$Camera.Substring(1)]
	$response=Rpc 'tools/call' @{name='camera.capture_object';arguments=@{object=$cameraIds[$Camera];includeDisabled=$false;width=$width;height=$height;format='image/png';freshness='nextFrame';imageStage=$Stage;includeOverlays=$false;allowBackendFallback=$false;timeoutMs=30000}}
	$text=@($response.result.content|Where-Object type -eq 'text');Require ($text.Count-eq1) 'Capture omitted unique native envelope.'
	$envelope=$text[0].text|ConvertFrom-Json -AsHashtable;Require ($envelope.ok-eq$true) "Native authoring capture failed: $($text[0].text)"
	$data=$envelope.data
	Require ($data.backendReadback-and!$data.deterministicFallback-and$data.imageStage-ceq$Stage) 'Synthetic, missing or wrong-stage image is forbidden.'
	Require ($data.source.objectId-ceq$cameraIds[$Camera]-and$data.width-eq$width-and$data.height-eq$height) 'Capture camera/extent mismatch.'
	Require ([uint64]$data.frameId-ge[uint64]$snapshot.frameId-and[uint64]$data.settingsRevision-eq[uint64]$snapshot.revision) 'Capture did not consume the exact effective checkpoint revision.'
	Require ([uint64]$data.backendTarget.targetGeneration-gt0-and[uint64]$data.backendTarget.rendererSessionGeneration-eq[uint64]$snapshot.rendererSessionGeneration-and$data.backendTarget.dedicatedCaptureTarget) 'Capture lacks its actual retained target/session.'
	$images=@($response.result.content|Where-Object{$_.type-ceq'image'-and$_.mimeType-ceq'image/png'});Require ($images.Count-eq1) 'Capture omitted one real PNG.'
	$bytes=[Convert]::FromBase64String($images[0].data);Require ($bytes.Length-gt8-and[BitConverter]::ToString($bytes,0,8)-ceq'89-50-4E-47-0D-0A-1A-0A') 'Native capture PNG is invalid.'
	$name=$Report.captureTag+'-'+$Camera+'-'+$Stage+'.png';$path=Join-Path $Root $name;[IO.File]::WriteAllBytes($path,$bytes)
	$row=@{tag=$Report.captureTag;camera=$Camera;cameraUUID=$snapshot.cameraId;stage=$Stage;png=$name;frameId=$data.frameId;settingsRevision=$data.settingsRevision;expectedSettingsRevision=$snapshot.revision;sourceEncoding=$data.sourceEncoding;deliveredEncoding=$data.deliveredEncoding;width=$width;height=$height;backendReadback=$true;deterministicFallback=$false;backendTarget=$data.backendTarget;sha256=(Pin $path).sha256;snapshot=$snapshot}
	WriteJson ([IO.Path]::ChangeExtension($path,'.json')) $row;return $row
}
function CompareImages([string]$Left,[string]$Right,[string]$Encoding) {
	Require ($Encoding-cin@('linear','srgb')) 'Unknown PNG transfer cannot be compared.'
	Add-Type -AssemblyName System.Drawing.Common
	$a=[Drawing.Bitmap]::new($Left);$b=[Drawing.Bitmap]::new($Right)
	try{
		Require ($a.Width-eq$width-and$a.Height-eq$height-and$b.Width-eq$width-and$b.Height-eq$height) 'Image comparison extent mismatch.'
		$sum=0.0;$count=0;$energy=0.0
		for($y=0;$y-lt$height;$y+=4){for($x=0;$x-lt$width;$x+=4){$pa=$a.GetPixel($x,$y);$pb=$b.GetPixel($x,$y);foreach($channel in @('R','G','B')){$u=[double]$pa.$channel/255;$v=[double]$pb.$channel/255;if($Encoding-ceq'srgb'){$u=if($u-le.04045){$u/12.92}else{[Math]::Pow(($u+.055)/1.055,2.4)};$v=if($v-le.04045){$v/12.92}else{[Math]::Pow(($v+.055)/1.055,2.4)}};$sum+=[Math]::Abs($u-$v);$energy+=$u;++$count}}}
		return @{meanAbsoluteLinearError=$sum/$count;leftMeanLinear=$energy/$count;samples=$count}
	}finally{$a.Dispose();$b.Dispose()}
}
function VerifyLane([string]$Root,$Proof) {
	Require ($Proof.schemaVersion-eq1-and$Proof.runId-ceq$runId-and$Proof.status-ceq'passed') 'Missing actual terminal authoring lane evidence.'
	$observation=$Proof.settingsObservation;ValidateCheckpoint $observation $Proof.bootId
	foreach($name in $checks){Require ($name-cin$observation.checks) "Managed authoring fixture omitted check $name. Missing integration cannot pass."}
	# The Game settings controller and camera authoring controller allocate
	# request numbers independently. Only the latter emits effectiveFrameId.
	$authoringReceipts=@($observation.receipts|Where-Object{$_.ContainsKey('effectiveFrameId')})
	Require (@($authoringReceipts|Where-Object{$_.state-ceq'Applied'-and$_.reason-ceq'None'-and[uint64]$_.requestId-gt0-and[uint64]$_.effectiveRevision-gt0-and[uint64]$_.appliedFrameId-gt0-and[uint64]$_.effectiveFrameId-gt0}).Count-ge3) 'Custom toggles/parameter updates lack exact native Applied receipts.'
	$rows=@($Proof.captures)
	Require (@($Proof.checkpoints).Count-eq$tags.Count) 'Authoring lane omitted exact capture checkpoints.'
	$requests=[Collections.Generic.HashSet[string]]::new()
	foreach($checkpoint in $Proof.checkpoints){
		ValidateCheckpoint $checkpoint $Proof.bootId
		Require ($requests.Add([string]$checkpoint.requestId)-and[uint64]$checkpoint.requestId-gt0) 'Capture checkpoint reused or omitted its exact native request ID.'
		$receipts=@($authoringReceipts|Where-Object{$_.requestId-ceq$checkpoint.requestId-and$_.state-ceq'Applied'-and$_.reason-ceq'None'})
		Require ($receipts.Count-eq1-and[uint64]$receipts[0].effectiveFrameId-gt0) 'Capture phase lacks one exact native effective request receipt.'
		$acks=@($observation.captureAcknowledgements|Where-Object{$_.schemaVersion-eq1-and$_.runId-ceq$runId-and$_.bootId-ceq$Proof.bootId-and$_.tag-ceq$checkpoint.captureTag})
		Require ($acks.Count-eq1) 'Managed fixture did not observe the exact native capture acknowledgement.'
	}
	foreach($tag in $tags){foreach($camera in @('CameraA','CameraB')){foreach($stage in @('raw','processed','composited')){
		$match=@($rows|Where-Object{$_.tag-ceq$tag-and$_.camera-ceq$camera-and$_.stage-ceq$stage});Require ($match.Count-eq1) "Missing unique native $tag/$camera/$stage image."
		$row=$match[0];Require ($row.backendReadback-and!$row.deterministicFallback-and$row.cameraUUID-ceq$cameraIds[$camera]-and[uint64]$row.frameId-gt0-and[uint64]$row.settingsRevision-eq[uint64]$row.expectedSettingsRevision-and[uint64]$row.backendTarget.targetGeneration-gt0) 'Runtime capture lacks exact native provenance.'
		Require ($row.width-eq$width-and$row.height-eq$height-and$row.snapshot.cameraId-ceq$cameraIds[$camera]-and[uint64]$row.frameId-ge[uint64]$row.snapshot.frameId-and[uint64]$row.expectedSettingsRevision-eq[uint64]$row.snapshot.revision-and[uint64]$row.backendTarget.rendererSessionGeneration-eq[uint64]$row.snapshot.rendererSessionGeneration-and$row.backendTarget.sourceWidth-eq$width-and$row.backendTarget.sourceHeight-eq$height-and$row.backendTarget.dedicatedCaptureTarget) 'Runtime image does not match its retained camera revision, extent and native session.'
		$path=[IO.Path]::GetFullPath((Join-Path $Root $row.png));Require (Below $path $Root) 'Capture path escaped lane evidence.';Require ((Pin $path).sha256-ceq$row.sha256) 'Capture hash does not match its actual image.'
	}}}
	foreach($camera in @('CameraA','CameraB')){
		$baseline=@($rows|Where-Object{$_.tag-ceq'baseline'-and$_.camera-ceq$camera-and$_.stage-ceq'processed'})[0]
		foreach($tag in @('enabled','parameter-changed','disabled')){
			$current=@($rows|Where-Object{$_.tag-ceq$tag-and$_.camera-ceq$camera-and$_.stage-ceq'processed'})[0]
			Require ($baseline.deliveredEncoding-ceq$current.deliveredEncoding) 'Effect capture transfer changed.'
			$score=CompareImages (Join-Path $Root $baseline.png) (Join-Path $Root $current.png) $current.deliveredEncoding
			Require ($score.leftMeanLinear-ge.01) 'Imported chart is black; effect witness is invalid.'
			if($camera-ceq'CameraB'-or$tag-ceq'disabled'){Require ($score.meanAbsoluteLinearError-le.004) 'Disabled effect/camera isolation changed pixels.'}
			else{Require ($score.meanAbsoluteLinearError-ge.005) 'Enabled/changed custom pass did not change actual pixels.'}
			$summary.comparisons+=@{lane=$Proof.mode;camera=$camera;tag=$tag;score=$score}
			$rawBase=@($rows|Where-Object{$_.tag-ceq'baseline'-and$_.camera-ceq$camera-and$_.stage-ceq'raw'})[0];$rawCurrent=@($rows|Where-Object{$_.tag-ceq$tag-and$_.camera-ceq$camera-and$_.stage-ceq'raw'})[0]
			Require ($rawBase.deliveredEncoding-ceq$rawCurrent.deliveredEncoding-and(CompareImages (Join-Path $Root $rawBase.png) (Join-Path $Root $rawCurrent.png) $rawBase.deliveredEncoding).meanAbsoluteLinearError-le.004) 'Fullscreen effect changed the Raw scene stage.'
		}
	}
	$a=@($rows|Where-Object{$_.tag-ceq'enabled'-and$_.camera-ceq'CameraA'-and$_.stage-ceq'processed'})[0];$b=@($rows|Where-Object{$_.tag-ceq'parameter-changed'-and$_.camera-ceq'CameraA'-and$_.stage-ceq'processed'})[0]
	Require ((CompareImages (Join-Path $Root $a.png) (Join-Path $Root $b.png) $a.deliveredEncoding).meanAbsoluteLinearError-ge.005) 'Typed parameter change did not change actual output.'
}
try{
	foreach($path in $required){$summary.prerequisites+=@{path=$path;present=(Test-Path -LiteralPath $path -PathType Leaf)}}
	if($DryRun){
		# Input-only schema checks exercise no native request and make no GPU claim.
		$effect=NewEffect;Require ($effect.parameterBytes-eq20-and$effect.parameters[1].offsetBytes-eq16-and$effect.inputs[0].combinedSampler) 'Frozen fixture manifest differs from shader ABI.'
		$settings=ShaderImportSettings;Require ($settings.shaderUse-ceq'post-process'-and$settings.stage-ceq'ps'-and$settings.entryPoint-ceq'PSMain') 'Fullscreen shader import mode differs from its dedicated ABI.'
		WriteJson (Join-Path $work 'planned-effect.teposteffect') $effect
		WriteJson (Join-Path $work 'integration-contract.json') @{schemaVersion=1;requiredTags=$tags;requiredChecks=$checks;cameraIds=$cameraIds;shaderId=$shaderId;effectId=$effectId;profileId=$profileId;shaderImportSettings=$settings;compiledAssetRoot='Build/Assets';runtimeArgument='--validate-postprocess-authoring';nativeImageryRequired=$true}
		$summary.status='dry-run';$summary.failure='';$summary.integrationExecuted=$false
	}else{
		Require ([IO.Path]::GetFullPath($PSCommandPath)-ieq(Join-Path $install 'Tests/Tools/PostProcessing/TestInstalledPostProcessAuthoring.ps1')) 'Run the installed authoring collector.'
		foreach($path in $required){Require (Test-Path -LiteralPath $path -PathType Leaf) "Missing installed authoring integration prerequisite: $path";$summary.inputs+=,(Pin $path)}
		# Reuse the maintained ordinary glTF chart materializer, without launching
		# its color test. Replace only this disposable project's controller/identity.
		$materialized=Join-Path $work 'inputs';& $materializer -InstallRoot $install -OutputDirectory $materialized -MaterializeOnly
		$project=Join-Path $materialized 'PostProcessColorContract';$editorRoot=Join-Path $work 'editor';$null=[IO.Directory]::CreateDirectory($editorRoot)
		$scenePath=Join-Path $project 'Assets/Scenes/Startup.tescene';$scene=ReadJson $scenePath
		$scene.objects[0].uuid=$cameraIds.CameraA;$scene.objects[1].uuid=$cameraIds.CameraB;$scene.objects[2].uuid='75000000000000000000000000000003'
		$binding=@($scene.objects[2].components.Script.BindingsJson|ConvertFrom-Json -AsHashtable);Require ($binding.Count-eq1) 'Canonical controller binding changed.'
		$binding[0].className='CustomPostProcessFixture';$binding[0].bindingId='76000000000000000000000000000001';$scene.objects[2].components.Script.BindingsJson=ConvertTo-Json -InputObject $binding -Compress -Depth 30;WriteJson $scenePath $scene
		Remove-Item -LiteralPath (Join-Path $project 'Assets/Scripts/ColorSettingsFixture.cs')
		Copy-Item -LiteralPath (Join-Path $fixture 'Scripts/CustomPostProcessFixture.cs') -Destination (Join-Path $project 'Assets/Scripts')
		WriteJson (Join-Path $project 'game.json') @{name='PostProcessAuthoringContract';gameId=$runId;version='1.0.0';startupScene='Scenes/Startup.tescene'}
		# Export names the launcher and package using the project display name.
		# Keep that name in sync when repurposing the disposable color fixture.
		$settingsPath=Join-Path $project 'ProjectSettings/ProjectSettings.terra';$projectSettings=ReadJson $settingsPath
		$projectSettings.projectDisplayName='PostProcessAuthoringContract';WriteJson $settingsPath $projectSettings
		$assetDir=Join-Path $project 'Assets/PostProcessing';$null=[IO.Directory]::CreateDirectory($assetDir)
		Copy-Item -LiteralPath $shader -Destination (Join-Path $assetDir 'Tint.frag.hlsl')
		WriteJson (Join-Path $assetDir 'Tint.frag.hlsl.meta') @{guid=$shaderId;importerName='ShaderImporter';lastModified=0;sourcePath='PostProcessing/Tint.frag.hlsl';compiledPath='PostProcessing/Tint.frag.tspf';settings=(ShaderImportSettings)}
		WriteJson (Join-Path $assetDir 'Tint.teposteffect') (NewEffect)
		WriteJson (Join-Path $assetDir 'Shared.tepostprofile') @{assetType='TerraEngine.PostProcessProfile';schemaVersion=1;assetId=$profileId;revision='1';name='Shared authoring fixture';values=@();overrideMask=@();customEffects=@($effectId);overrideCustomEffects=$true}
		$envs=@{TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_ROOT=$editorRoot;TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_RUN_ID=$runId;TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_SHADER_ID=$shaderId;TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_EFFECT_ID=$effectId;TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_PROFILE_ID=$profileId;TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT=(Join-Path $editorRoot 'save-data');TERRAENGINE_SUPPRESS_PROJECT_RECENTS='1'}
		$prefs=Join-Path $editorRoot 'editor-preferences.json';WriteJson $prefs @{recentProjects=@();autoBuildScripts=$true;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;rendererDiagnosticsPreferencesInitialized=$true};$envs.TERRAENGINE_EDITOR_PREFS_SESSION_PATH=$prefs
		Require (@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue).Count-eq0) 'MCP port already occupied.'
		$owner=StartOwned $editor @('--project',$project) $editorRoot $envs
		$proof=@{schemaVersion=1;runId=$runId;mode='editor';status='running';captures=@();checkpoints=@();bootId=''}
		try{
			$clock=[Diagnostics.Stopwatch]::StartNew();$ready=$false
			do{try{$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue);Require ($listeners.Count-eq1-and$listeners[0].OwningProcess-eq$owner.process.Id) 'MCP listener does not belong to owned editor.';$hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='postprocess-authoring-installed';version='1'}};Require ($hello.result.serverInfo.name-ceq'TerraEngine Editor') 'Unexpected server.';$state=Tool 'project.state';if($state.hasProject-and[IO.Path]::GetFullPath($state.projectRoot)-ieq$project-and!$state.assetImportsActive-and$state.readiness.simulationEssentialReady-and$state.activeScenePath){$ready=$true;break}}catch{};Require (!$owner.process.HasExited) 'Editor exited before authoring readiness.';Start-Sleep -Milliseconds 250}while($clock.Elapsed.TotalSeconds-lt$TimeoutSeconds)
			Require $ready 'Installed editor authoring readiness timed out.'
			$shaderImport=ImportActual 'PostProcessing/Tint.frag.hlsl' 'ShaderImporter';Require ($shaderImport.metadata.guid.Replace('-','')-ieq$shaderId) 'Actual shader importer identity differs from effect dependency.'
			$null=ImportActual 'PostProcessing/Tint.teposteffect' 'PostProcessEffectImporter';$null=ImportActual 'PostProcessing/Shared.tepostprofile' 'PostProcessProfileImporter'
			$chart=Tool 'asset.import' @{path='Models/ColorChart.gltf';force=$true};$receipt=ReadJson (Join-Path $project 'Assets/Models/ColorChart.gltf.import.json');Require ($receipt.status-ceq'Succeeded'-and@($receipt.generatedPrefabs).Count-eq1) 'Ordinary chart import did not produce one prefab.'
			$instances=Tool 'prefab.instantiate_many' @{prefab=$receipt.generatedPrefabs[0].guid;count=2;columns=2;spacing=8;origin=@(4,0,4);namePrefix='ImportedAuthoringChart'};Require ($instances.instanceCount-eq2) 'Two actual chart instances missing.'
			$null=Tool 'scene.save';$null=Tool 'play.enter' @{expectedState='stopped'};AwaitPlay 'playing'
			$clock.Restart();$seen=@{};$finished=$false
			do{
				$observation=Join-Path $editorRoot 'custom-post-process-observation.json'
				if(Test-Path -LiteralPath $observation){$report=ReadJson $observation;ValidateCheckpoint $report $proof.bootId;if(!$proof.bootId){$proof.bootId=$report.bootId}
					if($report.phase-ceq'Capture'-and$report.captureTag-and!$seen.ContainsKey($report.captureTag)){
						Require ($seen.Count-lt$tags.Count-and$report.captureTag-ceq$tags[$seen.Count]) 'Unexpected or reordered authoring capture phase.';$proof.checkpoints+=,$report
						foreach($camera in @('CameraA','CameraB')){foreach($stage in @('raw','processed','composited')){$proof.captures+=,(CaptureActual $editorRoot $report $camera $stage)}}
						$seen[$report.captureTag]=$true;WriteJson (Join-Path $editorRoot ('custom-post-process-capture-ack-'+$report.captureTag+'.json')) @{schemaVersion=1;runId=$runId;bootId=$proof.bootId;tag=$report.captureTag}
					}
					if($report.status-ceq'passed'){$proof.settingsObservation=$report;$finished=$true;break}
				};Require (!$owner.process.HasExited) 'Editor exited before authoring completion.';Start-Sleep -Milliseconds 50
			}while($clock.Elapsed.TotalSeconds-lt$TimeoutSeconds)
			Require $finished 'Authoring managed/capture integration timed out.'
			# A failed ordinary shader reimport must retain the preceding compiled
			# payload. Restore the saved source only after retaining real diagnostics.
			# Mutate only the copied dedicated HLSL file: remove its final closing
			# brace. No embedded substitute shader or fabricated compiler failure.
			$validBytes=[IO.File]::ReadAllBytes($shader);$brace=[Array]::LastIndexOf($validBytes,[byte]125);Require ($brace-ge0) 'Bundled shader has no final closing brace to corrupt.'
			$invalidBytes=[byte[]]::new($validBytes.Length-1);[Array]::Copy($validBytes,0,$invalidBytes,0,$brace);[Array]::Copy($validBytes,$brace+1,$invalidBytes,$brace,$validBytes.Length-$brace-1)
			[IO.File]::WriteAllBytes((Join-Path $assetDir 'Tint.frag.hlsl'),$invalidBytes)
			$invalidWitness=Join-Path $editorRoot 'invalid-source-Tint.frag.hlsl';[IO.File]::WriteAllBytes($invalidWitness,$invalidBytes);$summary.inputs+=,(Pin $invalidWitness)
			$negative=Tool 'asset.import' @{path='PostProcessing/Tint.frag.hlsl';force=$true} -AllowFailure;WriteJson (Join-Path $editorRoot 'invalid-shader-reimport.json') $negative
			# asset.import's successful envelope means queued, not compiled. Require
			# actual settlement, retained cooked bytes, later compiler diagnostics,
			# and a usable completed view instead of inventing a terminal RPC status.
			AwaitImports
			Require ((Pin $shaderImport.compiled.path).sha256-ceq$shaderImport.compiled.sha256) 'Rejected shader import replaced the valid compiled payload.'
			$health=$report|ConvertTo-Json -Depth 100|ConvertFrom-Json -AsHashtable;$health.captureTag='invalid-import-retained'
			$healthCapture=CaptureActual $editorRoot $health 'CameraA' 'processed';$proof.captures+=,$healthCapture
			$disabled=@($proof.captures|Where-Object{$_.tag-ceq'disabled'-and$_.camera-ceq'CameraA'-and$_.stage-ceq'processed'})[0]
			Require ($disabled.deliveredEncoding-ceq$healthCapture.deliveredEncoding-and(CompareImages (Join-Path $editorRoot $disabled.png) (Join-Path $editorRoot $healthCapture.png) $disabled.deliveredEncoding).meanAbsoluteLinearError-le.004) 'Rejected shader import damaged the preceding usable view.'
			Copy-Item -LiteralPath $shader -Destination (Join-Path $assetDir 'Tint.frag.hlsl') -Force;$null=ImportActual 'PostProcessing/Tint.frag.hlsl' 'ShaderImporter'
			$null=Tool 'play.exit';AwaitPlay 'stopped'
			$proof.status='passed';VerifyLane $editorRoot $proof
		}catch{$proof.status='failed';$proof.failure=$_.Exception.Message;throw}
		finally{$proof.lifecycle=StopOwned $owner;WriteJson (Join-Path $editorRoot 'editor-captures.json') $proof}
		Require (!$proof.lifecycle.forcedTermination-and$proof.lifecycle.exitCode-eq0-and!$proof.lifecycle.cleanupFailure) 'Editor did not settle cleanly.';$summary.lanes+=,$proof
		$importDiagnostics=(Get-Content -LiteralPath (Join-Path $editorRoot 'stdout.log') -Raw)+(Get-Content -LiteralPath (Join-Path $editorRoot 'stderr.log') -Raw)
		Require ($importDiagnostics-match'(?i)Tint\.frag\.hlsl'-and$importDiagnostics-match'(?i)(shader compiler.*(failed|exit)|import.*failed|dxc failed|error:)') 'Invalid shader reimport omitted its actual compiler/import diagnostic.'
		$exportRoot=Join-Path $work 'export';$package=Join-Path $work 'Package';$owner=StartOwned $exporter @('--project',$project,'--profile',(Join-Path $project 'BuildProfiles/color.json'),'--output',$package,'--evidence',$exportRoot,'--run-id',$runId) $exportRoot @{}
		try{Require ($owner.process.WaitForExit($TimeoutSeconds*1000)-and$owner.process.ExitCode-eq0) 'Installed authoring export failed.'}finally{$life=StopOwned $owner;WriteJson (Join-Path $exportRoot 'process.json') $life}
		Require (!$life.forcedTermination-and!$life.cleanupFailure) 'Exporter failed orderly completion.'
		# Resolve the launcher from this export's reported archive. The exporter can
		# derive its base name from the source directory, even with a display name.
		$exportResult=ReadJson (Join-Path $exportRoot 'export-result.json')
		$archive=[IO.Path]::GetFullPath($exportResult.package.archivePath)
		Require ($exportResult.success-and$exportResult.runId-ceq$runId-and[IO.Path]::GetDirectoryName($archive)-ieq$package-and[IO.Path]::GetExtension($archive)-ceq'.tepak') 'Exporter did not report an archive in the owned package.'
		$summary.inputs+=,(Pin $archive)
		$isolated=Join-Path $work 'IsolatedPackage';Copy-Item -LiteralPath $package -Destination $isolated -Recurse
		foreach($file in Get-ChildItem -LiteralPath $isolated -Recurse -File|Where-Object{$_.Extension-cin@('.exe','.dll','.tepak','.spv')}){$summary.inputs+=,(Pin $file.FullName)}
		# Resolve and verify both disposable paths before moving anything. The old
		# authored absolute project path then genuinely stops being available.
		$retired=Join-Path $work 'RetiredAuthoredProject';Require ((Below $project $work)-and(Below $retired $work)) 'Project retirement escaped evidence workspace.'
		Move-Item -LiteralPath $project -Destination $retired;Require (!(Test-Path -LiteralPath $project)) 'Original authored project remained available during isolated export.'
		$summary.projectRetirement=@{original=$project;retained=$retired;originalUnavailable=$true}
		foreach($pin in $summary.inputs){if(Below $pin.path $project){$pin.originalPath=$pin.path;$pin.path=$retired+$pin.path.Substring($project.Length)}}
		$runtimeRoot=Join-Path $work 'runtime';$envs.TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_ROOT=$runtimeRoot;$envs.TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT=Join-Path $runtimeRoot 'save-data'
		$launcher=Join-Path $isolated ([IO.Path]::GetFileNameWithoutExtension($archive)+'.exe');$summary.inputs+=,(Pin $launcher)
		$owner=StartOwned $launcher @('--validate-postprocess-authoring',$runtimeRoot) $runtimeRoot $envs
		try{Require ($owner.process.WaitForExit($TimeoutSeconds*1000)-and$owner.process.ExitCode-eq0) 'Isolated native authoring capture integration failed or is unavailable.'}finally{$life=StopOwned $owner;WriteJson (Join-Path $runtimeRoot 'process.json') $life}
		Require (!$life.forcedTermination-and!$life.cleanupFailure) 'Isolated runtime failed orderly completion.'
		$runtime=ReadJson (Join-Path $runtimeRoot 'runtime-captures.json');Require ($runtime.orderlyShutdown-and$runtime.mode-ceq'export') 'Runtime lacks actual orderly native completion.';VerifyLane $runtimeRoot $runtime;$summary.lanes+=,$runtime
		Require ($runtime.bootId-cne$proof.bootId) 'Export reused the editor process boot identity.'
		foreach($row in @($proof.captures|Where-Object{$_.tag-cin$tags})){
			$match=@($runtime.captures|Where-Object{$_.tag-ceq$row.tag-and$_.camera-ceq$row.camera-and$_.stage-ceq$row.stage});Require ($match.Count-eq1-and$match[0].deliveredEncoding-ceq$row.deliveredEncoding) 'Export lacks an exact matched stage/transfer.'
			$score=CompareImages (Join-Path $editorRoot $row.png) (Join-Path $runtimeRoot $match[0].png) $row.deliveredEncoding;Require ($score.meanAbsoluteLinearError-le.012) 'Editor/isolated-export effect pixels differ.'
			$summary.comparisons+=@{lane='editor-export';camera=$row.camera;tag=$row.tag;stage=$row.stage;score=$score}
		}
		# A separate disposable export is deliberately missing its real fragment
		# dependency. Its rejection cannot be substituted with the valid export.
		$missingProject=Join-Path $work 'MissingDependencyProject';Copy-Item -LiteralPath $retired -Destination $missingProject -Recurse
		$missingEffect=ReadJson (Join-Path $missingProject 'Assets/PostProcessing/Tint.teposteffect');$missingEffect.shaderAssetId=$missingId
		WriteJson (Join-Path $missingProject 'Assets/PostProcessing/Tint.teposteffect') $missingEffect
		$missingMetadata=ReadJson (Join-Path $missingProject 'Assets/PostProcessing/Tint.teposteffect.meta')
		$missingCompiled=ResolveImportedArtifact $missingProject ([string]$missingMetadata.compiledPath)
		Require (Test-Path -LiteralPath $missingCompiled -PathType Leaf) 'Missing-dependency fixture has no actual cooked effect at its metadata reference.'
		WriteJson $missingCompiled $missingEffect
		$negativeRoot=Join-Path $work 'missing-dependency-export';$owner=StartOwned $exporter @('--project',$missingProject,'--profile',(Join-Path $missingProject 'BuildProfiles/color.json'),'--output',(Join-Path $work 'RejectedPackage'),'--evidence',$negativeRoot,'--run-id',($runId+'-missing')) $negativeRoot @{}
		try{Require ($owner.process.WaitForExit($TimeoutSeconds*1000)-and$owner.process.ExitCode-ne0) 'Missing shader dependency export did not reject.'}finally{$life=StopOwned $owner;WriteJson (Join-Path $negativeRoot 'process.json') $life}
		$diagnostics=(Get-Content -LiteralPath (Join-Path $negativeRoot 'stdout.log') -Raw)+(Get-Content -LiteralPath (Join-Path $negativeRoot 'stderr.log') -Raw)
		Require (!$life.forcedTermination-and!$life.cleanupFailure-and$diagnostics-match'(?i)(missing|unresolved|unavailable|no ShaderImporter).*(shader|dependency)|(shader|dependency).*(missing|unresolved|unavailable|no ShaderImporter)') 'Missing dependency rejection omitted a useful native diagnostic.'
		$summary.missingDependencyExportRejected=$true;$summary.status='passed-custom-authoring'
	}
}catch{
	$summary.status='failed';$summary.failure=$_.Exception.Message
	if($failedImportRelative){
		# The inner owned-process finally has now drained compiler stdout/stderr.
		# Surface the actual rejection, not only the resulting absent artifact.
		$diagnostics=@(ReadImportDiagnostics $editorRoot $failedImportRelative)
		$summary.failedImport=@{source=$failedImportRelative;compilerDiagnostics=$diagnostics;terminalImportStatusInvented=$false}
		if($diagnostics.Count){$summary.failure+="`nActual compiler/import diagnostics:`n"+($diagnostics-join"`n")}
	}
}
finally{WriteJson (Join-Path $work 'authoring-acceptance.json') $summary}
if($summary.status-cnotin@('dry-run','passed-custom-authoring')){throw $summary.failure}
Write-Output (Join-Path $work 'authoring-acceptance.json')
