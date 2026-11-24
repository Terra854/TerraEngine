<#
Requires PowerShell7, an installed BUILD_TESTING tree and unlocked Vulkan desktop.
Run the installed copy with -InstallRoot <install> -OutputDirectory <fresh-path>.
Creates a disposable authored project, imports ordinary glTF/TGA assets through
real editor tools, captures native finished-image stages, then exports the exact
saved scene and invokes the test-build runtime capture seam. No personal project
or global preferences are opened. -MaterializeOnly prepares inputs and reports
prepared, never passed. Static cameras permit stage comparisons across explicitly
recorded distinct completed frame IDs; no same-frame claim is made.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$OutputDirectory,
	[ValidateRange(1024,65535)][int]$McpPort=43217,
	[ValidateRange(60,1800)][int]$TimeoutSeconds=900,
	[switch]$MaterializeOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$install=[IO.Path]::GetFullPath($InstallRoot);$work=[IO.Path]::GetFullPath($OutputDirectory)
function Require([bool]$Condition,[string]$Message) { if(!$Condition){throw $Message} }
function Below([string]$Child,[string]$Parent) { return $Child.StartsWith($Parent.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) }
Require (!(Test-Path -LiteralPath $work)) 'Evidence directory must be fresh.'
Require ($work -ine $install -and !(Below $work $install) -and !(Below $install $work)) 'Install and evidence trees must be disjoint.'
$editor=Join-Path $install 'Editor/TerraEngine-Editor.exe';$exporter=Join-Path $install 'Editor/TerraEngine-Export.exe'
$template=Join-Path $install 'Tests/Tools/Correctness/Fixtures/ManagedExportProject'
$fixture=Join-Path $install 'Tests/Fixtures/PostProcessing'
foreach($path in @($editor,$exporter,(Join-Path $template 'Assets/Scenes/Startup.tescene'),(Join-Path $fixture 'Scripts/ColorSettingsFixture.cs'),(Join-Path $fixture 'Assembly-CSharp.csproj'))) {Require (Test-Path -LiteralPath $path -PathType Leaf) "Missing installed prerequisite: $path"}
if(!$MaterializeOnly){Require ([IO.Path]::GetFullPath($PSCommandPath) -ieq (Join-Path $install 'Tests/Tools/PostProcessing/TestInstalledPostProcessColor.ps1')) 'Run the installed runner.'}
$null=[IO.Directory]::CreateDirectory($work)
$runId='color-'+[Guid]::NewGuid().ToString('N')
$summary=[ordered]@{schemaVersion=1;runId=$runId;status='running';failure='';installRoot=$install;inputs=@();lanes=@();comparisons=@()}
function WriteJson([string]$Path,$Value) {
	$temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
	[IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 100),[Text.UTF8Encoding]::new($false))
	[IO.File]::Move($temporary,$Path,$true)
}
function ReadJson([string]$Path) {
	# Delete sharing permits the fixture's atomic evidence replacement.
	$stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
	$reader=[IO.StreamReader]::new($stream)
	try{return $reader.ReadToEnd()|ConvertFrom-Json -AsHashtable}finally{$reader.Dispose()}
}
function Pin([string]$Path) {Require (Test-Path -LiteralPath $Path -PathType Leaf) "Missing artifact: $Path";return @{path=[IO.Path]::GetFullPath($Path);length=(Get-Item -LiteralPath $Path).Length;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}}
$project=Join-Path $work 'PostProcessColorContract'
$cameraIds=@{CameraA='73000000000000000000000000000001';CameraB='73000000000000000000000000000002'}
$curveId='73000000000000000000000000000010';$lutId='73000000000000000000000000000011'
$width=1280;$height=720
# Fixed normalized masks are authored from projection, never selected from an
# observed output. The halo rectangle explicitly excludes the emitter core.
$masks=@{gray=@(.18,.30,.20,.40);color=@(.51,.32,.10,.36);emitter=@(.71,.44,.07,.12);emitterWitness=@(.724,.47,.04,.06);halo=@(.68,.35,.13,.30)}
function NewTga([string]$Path,[int]$Width,[int]$Height,[scriptblock]$Pixel) {
	$stream=[IO.File]::Create($Path);$writer=[IO.BinaryWriter]::new($stream)
	try {
		$header=[byte[]]::new(18);$header[2]=2;$header[12]=[byte]($Width-band255);$header[13]=[byte]($Width-shr8);$header[14]=[byte]($Height-band255);$header[15]=[byte]($Height-shr8);$header[16]=32;$header[17]=40;$writer.Write($header)
		for($y=0;$y-lt$Height;$y++){for($x=0;$x-lt$Width;$x++){$rgba=&$Pixel $x $y;$writer.Write([byte]$rgba[2]);$writer.Write([byte]$rgba[1]);$writer.Write([byte]$rgba[0]);$writer.Write([byte]$rgba[3])}}
	}finally{$writer.Dispose()}
}
function NewChart([string]$Path) {
	$memory=[IO.MemoryStream]::new();$writer=[IO.BinaryWriter]::new($memory)
	$views=[Collections.Generic.List[object]]::new();$accessors=[Collections.Generic.List[object]]::new();$materials=[Collections.Generic.List[object]]::new();$primitives=[Collections.Generic.List[object]]::new()
	$patches=[Collections.Generic.List[object]]::new()
	$patches.Add(@{x0=-3.4;x1=3.4;y0=-1.9;y1=1.9;z=-.01;rgb=@(0,0,0);strength=1})
	$levels=@(.02,.04,.08,.16,.32,.64,1,2)
	for($i=0;$i-lt8;$i++){$value=$levels[$i];$factor=[Math]::Min(1.0,[double]$value);$patches.Add(@{x0=-2.8+$i*.25;x1=-2.55+$i*.25;y0=-1.2;y1=1.2;z=0;rgb=@($factor,$factor,$factor);strength=[Math]::Max(1.0,[double]$value)})}
	for($i=0;$i-lt3;$i++){$rgb=@(.05,.05,.05);$rgb[$i]=.7;$patches.Add(@{x0=0;x1=1;y0=-1.2+$i*.8;y1=-.4+$i*.8;z=0;rgb=$rgb;strength=1})}
	$patches.Add(@{x0=1.8;x1=2.2;y0=-.25;y1=.25;z=0;rgb=@(1,1,1);strength=8})
	try {
		foreach($p in $patches){
			while($memory.Position%4){$writer.Write([byte]0)};$offset=$memory.Position
			foreach($v in @(@($p.x0,$p.y0,$p.z),@($p.x1,$p.y0,$p.z),@($p.x1,$p.y1,$p.z),@($p.x0,$p.y1,$p.z))){foreach($n in $v){$writer.Write([single]$n)}}
			$positionView=$views.Count;$views.Add(@{buffer=0;byteOffset=$offset;byteLength=48;target=34962})
			$position=$accessors.Count;$accessors.Add(@{bufferView=$positionView;componentType=5126;count=4;type='VEC3';min=@($p.x0,$p.y0,$p.z);max=@($p.x1,$p.y1,$p.z)})
			$offset=$memory.Position;for($v=0;$v-lt4;$v++){$writer.Write([single]0);$writer.Write([single]0);$writer.Write([single]1)}
			$normalView=$views.Count;$views.Add(@{buffer=0;byteOffset=$offset;byteLength=48;target=34962})
			$normal=$accessors.Count;$accessors.Add(@{bufferView=$normalView;componentType=5126;count=4;type='VEC3'})
			$offset=$memory.Position;foreach($index in @(0,1,2,0,2,3)){$writer.Write([uint16]$index)}
			$indexView=$views.Count;$views.Add(@{buffer=0;byteOffset=$offset;byteLength=12;target=34963})
			$indices=$accessors.Count;$accessors.Add(@{bufferView=$indexView;componentType=5123;count=6;type='SCALAR'})
			$material=$materials.Count;$materials.Add(@{name="ChartPatch$material";doubleSided=$true;pbrMetallicRoughness=@{baseColorFactor=@(0,0,0,1);metallicFactor=0;roughnessFactor=1};emissiveFactor=$p.rgb;extensions=@{KHR_materials_emissive_strength=@{emissiveStrength=$p.strength}}})
			$primitives.Add(@{attributes=@{POSITION=$position;NORMAL=$normal};indices=$indices;material=$material;mode=4})
		}
		$writer.Flush();$binary=$memory.ToArray()
		# One material per ordinary mesh avoids relying on a merged multi-material
		# submesh binding. The same source triangles/positions remain unchanged.
		$meshes=[Collections.Generic.List[object]]::new();$nodes=[Collections.Generic.List[object]]::new()
		for($patch=0;$patch-lt$primitives.Count;$patch++){$meshes.Add(@{name=('ChartPatch'+$patch);primitives=@($primitives[$patch])})}
		for($patch=0;$patch-lt$primitives.Count;$patch++){$nodes.Add(@{name=('ChartPatch'+$patch);mesh=$patch})}
		WriteJson $Path @{asset=@{version='2.0';generator='TerraEngine installed color regression ordinary asset'};extensionsUsed=@('KHR_materials_emissive_strength');buffers=@(@{uri='data:application/octet-stream;base64,'+[Convert]::ToBase64String($binary);byteLength=$binary.Length});bufferViews=$views.ToArray();accessors=$accessors.ToArray();materials=$materials.ToArray();meshes=$meshes.ToArray();nodes=$nodes.ToArray();scenes=@(@{nodes=@(0..($nodes.Count-1))});scene=0}
	}finally{$writer.Dispose()}
}
function Materialize {
	foreach($directory in @('Assets/Scenes','Assets/Scripts','Assets/Models','Assets/Textures','ProjectSettings','BuildProfiles','Build/Assembly-CSharp/References')){$null=[IO.Directory]::CreateDirectory((Join-Path $project $directory))}
	$scene=ReadJson (Join-Path $template 'Assets/Scenes/Startup.tescene')
	$a=$scene.objects[0];$a.name='CameraA';$a.uuid=$cameraIds.CameraA;$a.components.Transform.WorldPose.position.offsetMetres=@(0,0,4);$a.components.Transform.ParentRelativePose.translationMetres=@(0,0,4)
	$b=$a|ConvertTo-Json -Depth 100|ConvertFrom-Json -AsHashtable;$b.name='CameraB';$b.uuid=$cameraIds.CameraB;$b.components.Transform.WorldPose.position.offsetMetres=@(8,0,4);$b.components.Transform.ParentRelativePose.translationMetres=@(8,0,4)
	$controller=$scene.objects[1];$controller.name='ColorSettingsFixture';$controller.uuid='73000000000000000000000000000003'
	$bindings=@($controller.components.Script.BindingsJson|ConvertFrom-Json -AsHashtable);Require ($bindings.Count-eq1) 'Canonical script binding changed.'
	$bindings[0].className='ColorSettingsFixture';$bindings[0].bindingId='74000000000000000000000000000001';$controller.components.Script.BindingsJson=ConvertTo-Json -InputObject $bindings -Depth 30 -Compress
	$scene.name='Post Process Color Contract';$scene.objects=@($a,$b,$controller);WriteJson (Join-Path $project 'Assets/Scenes/Startup.tescene') $scene
	WriteJson (Join-Path $project 'game.json') @{name='PostProcessColorContract';gameId=$runId;version='1.0.0';startupScene='Scenes/Startup.tescene'}
	WriteJson (Join-Path $project 'ProjectSettings/ProjectSettings.terra') @{projectDisplayName='PostProcessColorContract'}
	Copy-Item -LiteralPath (Join-Path $template 'ProjectSettings/RenderSettings.json') -Destination (Join-Path $project 'ProjectSettings/RenderSettings.json')
	Copy-Item -LiteralPath (Join-Path $template 'BuildProfiles/correctness.json') -Destination (Join-Path $project 'BuildProfiles/color.json')
	Copy-Item -LiteralPath (Join-Path $fixture 'Assembly-CSharp.csproj') -Destination $project
	Copy-Item -LiteralPath (Join-Path $fixture 'Scripts/ColorSettingsFixture.cs') -Destination (Join-Path $project 'Assets/Scripts')
	foreach($reference in @(@('Managed/TerraEngine.CSharp.dll','TerraEngine.CSharp.dll'),@('Managed/Analyzers/TerraEngine.Export.Generator.dll','TerraEngine.Export.Generator.dll'))){Copy-Item -LiteralPath (Join-Path (Split-Path $editor) $reference[0]) -Destination (Join-Path $project ('Build/Assembly-CSharp/References/'+$reference[1]))}
	NewChart (Join-Path $project 'Assets/Models/ColorChart.gltf')
	NewTga (Join-Path $project 'Assets/Textures/GradeCurve.tga') 32 1 {param($x,$y)$t=$x/31.0;$q=[int][Math]::Round(255*$t*$t);@($q,$q,$q,[int][Math]::Round(255*$t))}
	NewTga (Join-Path $project 'Assets/Textures/GradeLut.tga') 256 16 {param($x,$y)$r=$x%16;$b=[int][Math]::Floor($x/16);@([int][Math]::Round(255*$b/15),[int][Math]::Round(255*$y/15),[int][Math]::Round(255*$r/15),255)}
	foreach($asset in @(@('GradeCurve',$curveId),@('GradeLut',$lutId))){WriteJson (Join-Path $project ('Assets/Textures/'+$asset[0]+'.tga.meta')) @{guid=$asset[1];importerName='TextureImporter';lastModified=0;sourcePath=('Textures/'+$asset[0]+'.tga');compiledPath=('Textures/'+$asset[0]+'.tetf');settings=@{semantic='data';storageFormat='rgba8';generateMipmaps=$false;quality=3;filterType='Box'}}}
	WriteJson (Join-Path $work 'authored-masks.json') @{schemaVersion=1;width=$width;height=$height;cameraIds=$cameraIds;masks=$masks;assetPolicy='ordinary glTF/TGA sources, actual installed importer required'}
}
$rpcId = 0
function Rpc([string]$Method, $Parameters) {
	$request = @{jsonrpc='2.0';id=(++$script:rpcId);method=$Method;params=$Parameters}
	# Cold native preparation may legitimately approach its 30-second request
	# deadline. Transport must not abandon that real capture after 10 seconds.
	# Never retry a timed-out mutation: its acceptance may already be visible.
	$transportTimeout = 10
	if($Method -ceq 'tools/call' -and $Parameters.name -ceq 'camera.capture_object') { $transportTimeout = 45 }
	elseif($Method -ceq 'tools/call' -and $Parameters.name -ceq 'asset.import') { $transportTimeout = [Math]::Min($TimeoutSeconds,180) }
	$response = Invoke-RestMethod -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request | ConvertTo-Json -Depth 30 -Compress) -TimeoutSec $transportTimeout
	# Store each real PNG once. The RPC transcript retains native text metadata
	# and a digest/length witness, never another copy of its base64 image payload.
	$loggedResponse = $response
	if($Method -ceq 'tools/call' -and $Parameters.name -ceq 'camera.capture_object' -and $null -eq $response.PSObject.Properties['error']) {
		$content = foreach($block in $response.result.content) {
			if($block.type -ceq 'image') {
				$bytes = [Convert]::FromBase64String($block.data)
				@{type='image';mimeType=$block.mimeType;byteLength=$bytes.Length;sha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()}
			} else { $block }
		}
		$loggedResponse = @{jsonrpc=$response.jsonrpc;id=$response.id;result=@{content=@($content)}}
	}
	[IO.File]::AppendAllText((Join-Path $work 'rpc.jsonl'),((@{request=$request;response=$loggedResponse} | ConvertTo-Json -Depth 60 -Compress) + "`n"))
	Require ($null -eq $response.PSObject.Properties['error']) 'Owned editor returned a JSON-RPC error.'
	return $response
}
function Tool([string]$Name, $Arguments=@{}) {
	$response = Rpc 'tools/call' @{name=$Name;arguments=$Arguments}
	$texts = @($response.result.content | Where-Object type -eq 'text')
	Require ($texts.Count -eq 1) "Tool $Name omitted its unique native envelope."
	$envelope = $texts[0].text | ConvertFrom-Json -AsHashtable
	Require ($envelope.ok -eq $true) "Native tool failed: $Name"
	return $envelope.data
}
function AwaitPlay([string]$Expected) {
	$clock = [Diagnostics.Stopwatch]::StartNew()
	do {
		$state = Tool 'play.state'
		if ($state.currentState -ceq $Expected -and $null -eq $state.pendingTransition) { return }
		Start-Sleep -Milliseconds 100
	} while ($clock.Elapsed.TotalSeconds -lt 90)
	throw "Owned editor did not reach Play state $Expected."
}
function StartOwned([string]$Executable, [string[]]$Arguments, [string]$LaneRoot, [hashtable]$Environment) {
	$start = [Diagnostics.ProcessStartInfo]::new($Executable)
	$start.WorkingDirectory = Split-Path -Parent $Executable
	$start.UseShellExecute = $false; $start.CreateNoWindow = $true
	$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
	$start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
	foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
	foreach ($key in $Environment.Keys) { $start.Environment[$key] = [string]$Environment[$key] }
	$process = [Diagnostics.Process]::Start($start)
	$out = [IO.File]::Create((Join-Path $LaneRoot 'stdout.log'))
	$err = [IO.File]::Create((Join-Path $LaneRoot 'stderr.log'))
	return @{process=$process;stdout=$out;stderr=$err;outTask=$process.StandardOutput.BaseStream.CopyToAsync($out);errTask=$process.StandardError.BaseStream.CopyToAsync($err)}
}
function StopOwned($Owner) {
	# Cleanup always returns diagnostics. A failed capture/export exception must
	# retain its original cause even when stream draining or disposal also fails.
	$result=@{forcedTermination=$false;exitCode=-1;cleanupFailure=''}
	try {
		if(!$Owner.process.HasExited) {
			$null=$Owner.process.CloseMainWindow()
			if(!$Owner.process.WaitForExit(10000)){$result.forcedTermination=$true;$Owner.process.Kill($true);$Owner.process.WaitForExit()}
		}
		$result.exitCode=$Owner.process.ExitCode
		$null=$Owner.outTask.GetAwaiter().GetResult();$null=$Owner.errTask.GetAwaiter().GetResult()
	}catch{$result.cleanupFailure=$_.Exception.Message}
	finally {
		foreach($resource in @($Owner.stdout,$Owner.stderr,$Owner.process)) {
			try{$resource.Dispose()}catch{$result.cleanupFailure+=('; disposal: '+$_.Exception.Message)}
		}
	}
	return $result
}

function ValueOf($Snapshot,[int]$Id) {
	$matches=@($Snapshot.values|Where-Object{[int]$_.id-eq$Id});Require ($matches.Count-eq1) "Missing unique setting $Id."
	return [string]$matches[0].value
}
function Active($Snapshot) {
	return ([uint32](ValueOf $Snapshot 67)-ne0 -or [double]::Parse((ValueOf $Snapshot 65),[Globalization.CultureInfo]::InvariantCulture)-ne0 -or [uint32](ValueOf $Snapshot 72)-ne0 -or (ValueOf $Snapshot 73)-ceq'True' -or ((ValueOf $Snapshot 84)-ceq'True' -and [double]::Parse((ValueOf $Snapshot 86),[Globalization.CultureInfo]::InvariantCulture)-gt0))
}
function CheckCheckpoint($Report,[string]$ExpectedRun,[string]$ExpectedBoot='') {
	Require ($Report.schemaVersion-eq1 -and $Report.runId-ceq$ExpectedRun -and $Report.bootId.Length-gt0) 'Checkpoint identity/schema mismatch.'
	if($ExpectedBoot){Require ($Report.bootId-ceq$ExpectedBoot) 'Checkpoint boot changed within one process.'}
	Require ($Report.status-cne'failed') "Managed fixture failed: $($Report.failure)"
	# Ready is an asynchronous wait for first native publications. Its null or
	# revision-zero snapshots are progress, never qualified capture evidence.
	if($Report.phase-cne'Capture'-and$Report.status-cne'passed'){return}
	foreach($name in @('CameraA','CameraB')) {
		$snapshot=$Report[$name.Substring(0,1).ToLowerInvariant()+$name.Substring(1)]
		if($null-eq$snapshot){Require ($Report.phase-cne'Capture') 'Capture checkpoint omitted camera snapshot.';continue}
		Require ($snapshot.cameraId-ceq$cameraIds[$name] -and [uint64]$snapshot.revision-gt0 -and [uint64]$snapshot.frameId-gt0) 'Camera checkpoint identity/revision missing.'
		foreach($stamp in @('sceneGeneration','objectLifetimeGeneration','componentGeneration','bindingGeneration','viewLifetimeGeneration','rendererSessionGeneration','backendResourceGeneration')){Require ([uint64]$snapshot[$stamp]-gt0) "Missing actual camera stamp: $stamp"}
	}
}
function EditorCapture([string]$LaneRoot,$Report,[string]$Camera,[string]$Stage) {
	$snapshot=$Report[$Camera.Substring(0,1).ToLowerInvariant()+$Camera.Substring(1)]
	$response=Rpc 'tools/call' @{name='camera.capture_object';arguments=@{object=$cameraIds[$Camera];includeDisabled=$false;width=$width;height=$height;format='image/png';freshness='nextFrame';imageStage=$Stage;includeOverlays=$false;allowBackendFallback=$false;timeoutMs=30000}}
	$texts=@($response.result.content|Where-Object { $_.type -eq 'text' });Require ($texts.Count-eq1) 'Capture omitted unique native envelope.'
	$envelope=$texts[0].text|ConvertFrom-Json -AsHashtable;Require ($envelope.ok-eq$true) "Native $Stage capture failed: $($texts[0].text)"
	$data=$envelope.data
	Require ($data.backendReadback -and !$data.deterministicFallback -and $data.imageStage-ceq$Stage) 'Capture lacks actual requested backend stage.'
	Require ($data.source.objectId-ceq$cameraIds[$Camera] -and $data.width-eq$width -and $data.height-eq$height) 'Native camera/extent mismatch.'
	Require ([uint64]$data.frameId-ge[uint64]$snapshot.frameId) 'Capture predates checkpoint publication.'
	if($Stage-cne'legacy'){Require ([uint64]$data.settingsRevision-eq[uint64]$snapshot.revision) 'Capture did not consume exact checkpoint settings revision.'}
	else {Require (!(Active $snapshot) -and [uint64]$data.settingsRevision-eq0) 'Legacy stage is allowed only for the true disabled bypass.'}
	Require ($data.ContainsKey('backendTarget') -and [uint64]$data.backendTarget.targetGeneration-gt0 -and [uint64]$data.backendTarget.rendererSessionGeneration-eq[uint64]$snapshot.rendererSessionGeneration) 'Capture omitted matching native generation.'
	Require ($data.backendTarget.sourceWidth-eq$width -and $data.backendTarget.sourceHeight-eq$height -and $data.backendTarget.dedicatedCaptureTarget) 'Capture resampled an unrelated target.'
	$images=@($response.result.content|Where-Object{ $_.type-eq'image' -and $_.mimeType-eq'image/png'});Require ($images.Count-eq1) 'Capture omitted exactly one PNG.'
	$bytes=[Convert]::FromBase64String($images[0].data);Require ($bytes.Length-gt8 -and [BitConverter]::ToString($bytes,0,8)-ceq'89-50-4E-47-0D-0A-1A-0A') 'Invalid PNG bytes.'
	$name=$Report.captureTag+'-'+$Camera+'-'+$Stage+'.png';$path=Join-Path $LaneRoot $name;[IO.File]::WriteAllBytes($path,$bytes)
	$row=@{tag=$Report.captureTag;camera=$Camera;cameraUUID=$snapshot.cameraId;stage=$Stage;imageStage=$Stage;png=$name;frameId=$data.frameId;settingsRevision=$data.settingsRevision;expectedSettingsRevision=$snapshot.revision;sourceEncoding=$data.sourceEncoding;deliveredEncoding=$data.deliveredEncoding;width=$data.width;height=$data.height;backendTarget=$data.backendTarget;sha256=(Pin $path).sha256;snapshot=$snapshot}
	WriteJson ([IO.Path]::ChangeExtension($path,'.json')) $row
	return $row
}
function ReadPixels([string]$Path,[string]$Encoding) {
	Add-Type -AssemblyName System.Drawing.Common
	$bitmap=[Drawing.Bitmap]::new($Path)
	try {
		Require ($bitmap.Width-eq$width -and $bitmap.Height-eq$height) 'Decoded PNG dimensions differ.'
		$values=[Collections.Generic.List[double]]::new()
		# Identical fixed sparse raster covers the complete image and all masks.
		for($y=0;$y-lt$height;$y+=4){for($x=0;$x-lt$width;$x+=4){$pixel=$bitmap.GetPixel($x,$y);foreach($channel in @($pixel.R,$pixel.G,$pixel.B)){$v=[double]$channel/255;if($Encoding-ceq'srgb'){$v=if($v-le.04045){$v/12.92}else{[Math]::Pow(($v+.055)/1.055,2.4)}};$values.Add($v)}}}
		return ,$values.ToArray()
	}finally{$bitmap.Dispose()}
}
function AssertChartWitness([string]$Root,$Row) {
	# These analytic witness bounds apply only to the unprocessed baseline. They
	# establish actual ramp/material/emitter visibility before any effect score.
	$pixels=ReadPixels (Join-Path $Root $Row.png) $Row.deliveredEncoding
	$grayMin=[double]::PositiveInfinity;$grayMax=[double]::NegativeInfinity
	$colorRange=0.0;$colorCount=0;$emitterSum=0.0;$emitterCount=0;$index=0
	for($y=0;$y-lt$height;$y+=4){for($x=0;$x-lt$width;$x+=4){
		$rgb=@($pixels[$index],$pixels[$index+1],$pixels[$index+2]);$index+=3
		foreach($mask in @('gray','color','emitterWitness')){
			$m=$masks[$mask]
			if($x-lt$m[0]*$width-or$x-ge($m[0]+$m[2])*$width-or$y-lt$m[1]*$height-or$y-ge($m[1]+$m[3])*$height){continue}
			$minimum=[Math]::Min($rgb[0],[Math]::Min($rgb[1],$rgb[2]));$maximum=[Math]::Max($rgb[0],[Math]::Max($rgb[1],$rgb[2]))
			if($mask-ceq'gray'){$grayMin=[Math]::Min($grayMin,$minimum);$grayMax=[Math]::Max($grayMax,$maximum)}
			elseif($mask-ceq'color'){$colorRange+=$maximum-$minimum;++$colorCount}
			else{$emitterSum+=$rgb[0]+$rgb[1]+$rgb[2];++$emitterCount}
		}
	}}
	Require ($colorCount-gt0-and$emitterCount-gt0-and[double]::IsFinite($grayMin)) 'Fixed chart witness masks are empty.'
	$range=$grayMax-$grayMin;$chromatic=$colorRange/$colorCount;$emitter=$emitterSum/(3*$emitterCount)
	Require ($range-ge.05) 'Imported gray ramp is not visible in its fixed mask.'
	Require ($chromatic-ge.1) 'Imported color materials are not visible in their fixed mask.'
	Require ($emitter-ge.75) 'Imported HDR emitter is not visible in its fixed mask.'
	return @{camera=$Row.camera;stage=$Row.stage;frameId=$Row.frameId;settingsRevision=$Row.settingsRevision;targetGeneration=$Row.backendTarget.targetGeneration;grayRange=$range;colorChannelRange=$chromatic;emitterMean=$emitter}
}
function Difference([double[]]$Left,[double[]]$Right,[string]$Mask='all',[switch]$ExcludeEmitter) {
	Require ($Left.Length-eq$Right.Length) 'Pixel sampling lengths differ.'
	$sum=0.0;$count=0;$index=0
	for($y=0;$y-lt$height;$y+=4){for($x=0;$x-lt$width;$x+=4){
		$inside=$true
		if($Mask-cne'all'){$m=$masks[$Mask];$inside=$x-ge$m[0]*$width-and$x-lt($m[0]+$m[2])*$width-and$y-ge$m[1]*$height-and$y-lt($m[1]+$m[3])*$height}
		if($ExcludeEmitter){$m=$masks.emitter;if($x-ge$m[0]*$width-and$x-lt($m[0]+$m[2])*$width-and$y-ge$m[1]*$height-and$y-lt($m[1]+$m[3])*$height){$inside=$false}}
		foreach($channel in 0..2){if($inside){$sum+=[Math]::Abs($Left[$index]-$Right[$index]);++$count};++$index}
	}}
	Require ($count-gt0) 'Fixed mask contains no samples.'
	return $sum/$count
}
function ScoreLane([string]$Root,$Proof) {
	$rows=@($Proof.captures);$checkpoints=@($Proof.checkpoints);$cache=@{}
	$controls=@('exposure-legacy','exposure-fixed','exposure-automatic','exposure-limits','exposure-brighten','exposure-darken','tone-legacy','tone-neutral','tone-filmic','grade-off','grade-identity','grade-temperature','grade-tint','grade-contrast','grade-saturation','grade-lift','grade-gain','bloom-off','bloom-on','bloom-threshold','bloom-zero-intensity','bloom-intensity','bloom-spread','bloom-knee')
	$required=@('baseline','camera-a-plus-one','camera-b-minus-one','camera-a-reset','invalid-batch-preserved','controls-restored','grade-curve-known','grade-curve-zero','grade-lut-known','grade-lut-zero')
	foreach($tag in $controls){$required+=@(($tag+'-baseline'),$tag)}
	foreach($tag in $required){Require (@($checkpoints|Where-Object{$_.captureTag-ceq$tag}).Count-eq1) "Required checkpoint missing/duplicated: $tag"}
	Require ($Proof.settingsObservation.status-ceq'passed'-and@($Proof.settingsObservation.receipts).Count-gt0) 'Final native fixture receipt evidence missing.'
	foreach($check in @('camera-a-plus-one-isolated','camera-b-minus-one-isolated','finite-invalid-mixed-batch-rollback','grade-curve-unregistered-mixed-batch-rollback','grade-lut-unregistered-mixed-batch-rollback')){Require ($check-cin$Proof.settingsObservation.checks) "Missing native rollback/isolation check: $check"}
	foreach($row in $rows){
		$path=[IO.Path]::GetFullPath((Join-Path $Root $row.png));Require (Below $path $Root) 'Capture path escaped evidence root.'
		$row.sha256=(Pin $path).sha256
		Require ($row.deliveredEncoding-cin@('linear','srgb','legacy')) 'Missing declared output transfer.'
		$cache[$row.tag+'|'+$row.camera+'|'+$row.stage]=ReadPixels $path $row.deliveredEncoding
	}
	$checks=[Collections.Generic.List[object]]::new();$previousB=$null;$firstRaw=@{}
	foreach($camera in @('CameraA','CameraB')){
		$baseline=@($rows|Where-Object{$_.tag-ceq'baseline'-and$_.camera-ceq$camera-and$_.stage-cin@('legacy','raw')})
		Require ($baseline.Count-eq1) 'Missing unique unprocessed baseline chart witness.'
		$checks.Add(@{name='actual-imported-chart-visible';witness=(AssertChartWitness $Root $baseline[0])})
	}
	foreach($checkpoint in $checkpoints){
		$tag=$checkpoint.captureTag
		foreach($camera in @('CameraA','CameraB')){
			$snapshot=$checkpoint[$camera.Substring(0,1).ToLowerInvariant()+$camera.Substring(1)]
			$stages=if(Active $snapshot){@('raw','processed','composited')}else{@('legacy')}
			foreach($stage in $stages){
				$matches=@($rows|Where-Object{$_.tag-ceq$tag-and$_.camera-ceq$camera-and$_.stage-ceq$stage});Require ($matches.Count-eq1) "Missing unique actual stage $tag/$camera/$stage."
				$row=$matches[0]
				Require ($row.cameraUUID-ceq$snapshot.cameraId-and[uint64]$row.frameId-ge[uint64]$snapshot.frameId-and[uint64]$row.expectedSettingsRevision-eq[uint64]$snapshot.revision) 'Native capture predates or mismatches exact observed camera settings.'
				Require ([uint64]$row.backendTarget.rendererSessionGeneration-eq[uint64]$snapshot.rendererSessionGeneration-and[uint64]$row.backendTarget.targetGeneration-gt0-and$row.backendTarget.sourceWidth-eq$width-and$row.backendTarget.sourceHeight-eq$height) 'Native target generation/extent mismatch.'
				if($stage-cne'legacy'){Require ([uint64]$row.settingsRevision-eq[uint64]$snapshot.revision-and$row.sourceEncoding-cin@('linear','srgb')-and$row.deliveredEncoding-cin@('linear','srgb')) 'Explicit stage lacks exact revision and transfer.'}
			}
			if(Active $snapshot){
				$raw=$cache["$tag|$camera|raw"]
				if($firstRaw.ContainsKey($camera)){Require ((Difference $firstRaw[$camera] $raw)-le.004) "Raw scene changed with postprocess controls: $tag/$camera"}else{$firstRaw[$camera]=$raw}
				$error=Difference $cache["$tag|$camera|processed"] $cache["$tag|$camera|composited"]
				Require ($error-le.012) "Processed/composited transfer mismatch: $tag/$camera ($error)."
				foreach($mask in @('gray','color')){Require ((Difference $cache["$tag|$camera|processed"] $cache["$tag|$camera|composited"] $mask)-le.012) "Transfer mismatch inside fixed $mask mask: $tag/$camera"}
				$checks.Add(@{name='single-transfer-static-pair';tag=$tag;camera=$camera;meanAbsoluteLinearError=$error;tolerance=.012})
			}
		}
		$bStage=if(Active $checkpoint.cameraB){'processed'}else{'legacy'};$b=$cache["$tag|CameraB|$bStage"]
		if($null-ne$previousB-and$tag-cne'camera-b-minus-one'){$error=Difference $previousB $b;Require ($error-le.004) "CameraB image changed during isolated A update: $tag";$checks.Add(@{name='camera-b-image-isolation';tag=$tag;meanAbsoluteError=$error;tolerance=.004})};$previousB=$b
	}
	# These controls must contribute visible pixels. Identity/off/zero controls
	# are checked separately; adaptation speeds need time-varying evidence.
	$changes=@{'exposure-fixed'='gray';'tone-filmic'='gray';'grade-temperature'='color';'grade-tint'='color';'grade-contrast'='gray';'grade-saturation'='color';'grade-lift'='gray';'grade-gain'='gray';'grade-curve-known'='gray';'grade-lut-known'='color';'bloom-on'='halo';'bloom-intensity'='halo';'bloom-spread'='halo'}
	foreach($tag in $changes.Keys){
		$referenceTag=if($tag-ceq'grade-curve-known'){'grade-curve-zero'}elseif($tag-ceq'grade-lut-known'){'grade-lut-zero'}elseif($tag-cin@('grade-curve-zero','grade-lut-zero')){'controls-restored'}else{$tag+'-baseline'}
		$base="$referenceTag|CameraA|processed";$effect="$tag|CameraA|processed"
		Require ($cache.ContainsKey($base)-and$cache.ContainsKey($effect)) "Missing effect/reference pixels for $tag."
		$error=Difference $cache[$base] $cache[$effect] $changes[$tag] -ExcludeEmitter:($changes[$tag]-ceq'halo')
		Require ($error-ge.0005) "Required effect contributed no measured pixels: $tag ($error)."
		$checks.Add(@{name='effect-contribution';tag=$tag;mask=$changes[$tag];meanAbsoluteLinearDifference=$error;minimum=.0005})
	}
	foreach($tag in @('grade-off','grade-identity','grade-curve-zero','grade-lut-zero','bloom-off','bloom-zero-intensity')){
		$referenceTag=if($tag-ceq'grade-curve-known'){'grade-curve-zero'}elseif($tag-ceq'grade-lut-known'){'grade-lut-zero'}elseif($tag-cin@('grade-curve-zero','grade-lut-zero')){'controls-restored'}else{$tag+'-baseline'}
		$base="$referenceTag|CameraA|processed";$effect="$tag|CameraA|processed";Require ($cache.ContainsKey($base)-and$cache.ContainsKey($effect)) "Missing identity/reference pixels for $tag."
		$error=Difference $cache[$base] $cache[$effect];Require ($error-le.004) "Zero/off/identity effect changed output: $tag ($error).";$checks.Add(@{name='zero-off-identity';tag=$tag;meanAbsoluteLinearError=$error;tolerance=.004})
	}
	return $checks.ToArray()
}
function WaitEditor($Owner) {
	$clock=[Diagnostics.Stopwatch]::StartNew();$last=''
	do {try {
		Require (!$Owner.process.HasExited) 'Owned editor exited before readiness.'
		$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue);Require ($listeners.Count-eq1-and$listeners[0].OwningProcess-eq$Owner.process.Id) 'MCP port belongs to another process.'
		$hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='postprocess-color-installed';version='1'}};Require ($hello.result.serverInfo.name-ceq'TerraEngine Editor') 'Unexpected MCP server.'
		$state=Tool 'project.state';Require ($state.hasProject-and[IO.Path]::GetFullPath($state.projectRoot)-ieq$project) 'Owned project mismatch.'
		if(!$state.assetImportsActive-and$state.readiness.simulationEssentialReady-and$state.activeScenePath){return}
	}catch{$last=$_.Exception.Message};Start-Sleep -Milliseconds 250}while($clock.Elapsed.TotalSeconds-lt$TimeoutSeconds)
	throw "Editor readiness failed: $last"
}
function AssertChartIdentityBasis($Pose) {
	Require ($Pose.rotation.Count-eq4-and$Pose.scale.Count-eq3) 'Chart pose basis is incomplete.'
	foreach($index in 0..3){Require ($Pose.rotation[$index]-eq$(if($index-eq0){1}else{0})) 'Chart authored rotation changed.'}
	foreach($axis in 0..2){Require ($Pose.scale[$axis]-eq1) 'Chart authored scale changed.'}
}
function VerifySavedChartPlacements($Instances) {
	# Inspect the real scene saved by the editor. Import/source intentions alone
	# cannot prove that both independent native chart instances were positioned.
	$saved=ReadJson (Join-Path $project 'Assets/Scenes/Startup.tescene')
	Require ($Instances.Count-eq2) 'Expected two actual chart instance receipts.'
	$proof=[Collections.Generic.List[object]]::new();$firstBindings=$null
	foreach($index in 0..1){
		$instance=$Instances[$index];$expectedX=$index*8
		$roots=@($saved.objects|Where-Object{$_.uuid-ceq$instance.instanceId});Require ($roots.Count-eq1) 'Saved scene omitted exact native chart root identity.'
		$root=$roots[0];$transform=$root.components.Transform
		AssertChartIdentityBasis $transform.WorldPose;AssertChartIdentityBasis $transform.ParentRelativePose
		Require ($transform.WorldPose.position.cell.x-ceq'0'-and$transform.WorldPose.position.cell.y-ceq'0'-and$transform.WorldPose.position.cell.z-ceq'0') 'Chart root escaped canonical zero cell.'
		$world=$transform.WorldPose.position.offsetMetres;$relative=$transform.ParentRelativePose.translationMetres
		Require ($world[0]-eq$expectedX-and$world[1]-eq0-and$world[2]-eq0-and$relative[0]-eq$expectedX-and$relative[1]-eq0-and$relative[2]-eq0) 'Saved native chart root placement differs from its camera.'
		Require (@($root.children).Count-eq13) 'Each independent saved chart must retain 13 material patch children.'
		$materials=[Collections.Generic.HashSet[string]]::new();$bindings=[Collections.Generic.List[string]]::new();$children=[Collections.Generic.List[object]]::new()
		foreach($child in $root.children){
			Require ($child.components.ContainsKey('MeshRenderComponent')) 'Saved chart patch lacks its ordinary renderer binding.'
			Require (!$child.ContainsKey('children')-or@($child.children).Count-eq0) 'Chart patch nesting changed.'
			$renderer=$child.components.MeshRenderComponent
			$materials.Add([string]$renderer.materialId)|Out-Null;$bindings.Add(([string]$renderer.meshId+'|'+[string]$renderer.materialId))
			$pose=$child.components.Transform.ParentRelativePose;AssertChartIdentityBasis $pose
			$local=$pose.translationMetres
			Require ($pose.transformSpaceId-eq0-and$local.Count-eq3-and$local[0]-eq0-and$local[1]-eq0-and$local[2]-eq0) 'Chart patch authored parent-relative placement changed.'
			# A nested child's serialized WorldPose can be a derived cache. Its
			# authoring authority is this exact parent relation and local pose.
			# Keep that cache as diagnostic data; actual B pixels must independently
			# prove the renderer used the predicted translated chart before ACK.
			$children.Add(@{uuid=$child.uuid;parentInstanceId=$instance.instanceId;parentRelativePosition=$local;predictedWorldPosition=@($expectedX,0,0);cachedWorldPosition=$child.components.Transform.WorldPose.position.offsetMetres})
		}
		Require ($materials.Count-eq13) 'Saved chart patch material identities collapsed.'
		$orderedBindings=@($bindings.ToArray()|Sort-Object)-join','
		if($index-eq0){$firstBindings=$orderedBindings}else{Require ($orderedBindings-ceq$firstBindings) 'Independent chart instances do not bind the same authored mesh/material pairs.'}
		$proof.Add(@{instanceId=$instance.instanceId;name=$root.name;worldPosition=$world;parentRelativePosition=$relative;patchChildren=13;uniqueMaterials=$materials.Count;children=$children.ToArray();predictionAuthority='root WorldPose plus nested ParentRelativePose';actualImageWitnessRequired=$true})
	}
	return $proof.ToArray()
}
function RunEditor([string]$Root) {
	$null=[IO.Directory]::CreateDirectory($Root);$prefs=Join-Path $Root 'editor-preferences.json'
	WriteJson $prefs @{recentProjects=@();autoBuildScripts=$true;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;rendererDiagnosticsPreferencesInitialized=$true}
	$envs=@{TERRAENGINE_EDITOR_PREFS_SESSION_PATH=$prefs;TERRAENGINE_SUPPRESS_PROJECT_RECENTS='1';TERRAENGINE_COLOR_FIXTURE_ROOT=$Root;TERRAENGINE_COLOR_FIXTURE_RUN_ID=$runId;TERRAENGINE_COLOR_FIXTURE_CURVE_ID=$curveId;TERRAENGINE_COLOR_FIXTURE_LUT_ID=$lutId;TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT=(Join-Path $Root 'save-data')}
	Require (@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue).Count-eq0) 'MCP port already occupied.'
	$owner=StartOwned $editor @('--project',$project) $Root $envs;$proof=@{mode='editor';pid=$owner.process.Id;runId=$runId;status='running';captures=@();checkpoints=@()}
	try {
		WaitEditor $owner
		$source=Join-Path $project 'Assets/Models/ColorChart.gltf';$before=(Pin $source).sha256
		$import=Tool 'asset.import' @{path='Models/ColorChart.gltf';force=$true};WriteJson (Join-Path $Root 'chart-import-tool.json') $import
		$deadline=[Diagnostics.Stopwatch]::StartNew();$receipt=$null
		do{if(Test-Path -LiteralPath ($source+'.import.json')){$candidate=ReadJson ($source+'.import.json');if($candidate.status-ceq'Succeeded'){$receipt=$candidate;break}};Require (!$owner.process.HasExited) 'Editor exited during chart import.';Start-Sleep -Milliseconds 100}while($deadline.Elapsed.TotalSeconds-lt$TimeoutSeconds)
		Require ($null-ne$receipt-and@($receipt.generatedPrefabs).Count-eq1-and(Pin $source).sha256-ceq$before) 'Actual chart import did not produce one prefab from unchanged input.'
		WriteJson (Join-Path $Root 'chart-import-receipt.json') $receipt
		Require (@($receipt.generatedMeshes).Count-eq13-and@($receipt.generatedPrefabs[0].materialGuids).Count-eq13) 'Chart import did not preserve each patch material binding.'
		Require (@($receipt.generatedMaterials|Where-Object{$_.emissiveFactor[0]-ge8-and$_.emissiveFactor[1]-ge8-and$_.emissiveFactor[2]-ge8}).Count-eq1) 'Actual imported material metadata omitted the HDR emitter.'
		# Native grid placement is centered in both axes. This origin gives exact
		# roots (0,0,0) and (8,0,0), matching CameraA and CameraB without live edits.
		$instantiation=Tool 'prefab.instantiate_many' @{prefab=$receipt.generatedPrefabs[0].guid;count=2;columns=2;spacing=8;origin=@(4,0,4);namePrefix='ImportedColorChart'}
		Require ($instantiation.instanceCount-eq2-and$instantiation.rootCount-eq2) 'Actual prefab batch did not create two chart roots.'
		WriteJson (Join-Path $Root 'chart-instantiation.json') $instantiation
		foreach($asset in @(@('GradeCurve',$curveId),@('GradeLut',$lutId))){$null=Tool 'asset.import' @{path=('Textures/'+$asset[0]+'.tga');force=$true};$metadata=ReadJson (Join-Path $project ('Assets/Textures/'+$asset[0]+'.tga.meta'));Require ($metadata.guid.Replace('-','')-ieq$asset[1]-and$metadata.settings.semantic-ceq'data'-and$metadata.settings.storageFormat-ceq'rgba8') 'Actual lookup import changed authored UUID or linear RGBA8 request.'}
		$settled=[Diagnostics.Stopwatch]::StartNew()
		do{$state=Tool 'project.state';if(!$state.assetImportsActive){break};Require (!$owner.process.HasExited) 'Editor exited while settling imports.';Start-Sleep -Milliseconds 100}while($settled.Elapsed.TotalSeconds-lt$TimeoutSeconds)
		Require (!$state.assetImportsActive) 'Actual asset imports did not settle.'
		$null=Tool 'scene.save';WriteJson (Join-Path $Root 'saved-chart-placements.json') @(VerifySavedChartPlacements @($instantiation.instances));AwaitPlay 'stopped';$null=Tool 'play.enter' @{expectedState='stopped'};AwaitPlay 'playing'
		$clock=[Diagnostics.Stopwatch]::StartNew();$seen=@{};$boot='';$finished=$false
		do {
			$path=Join-Path $Root 'color-observation.json'
			if(Test-Path -LiteralPath $path){
				$report=ReadJson $path;CheckCheckpoint $report $runId $boot;if(!$boot){$boot=$report.bootId;$proof.bootId=$boot}
				if($report.status-ceq'passed'){$proof.settingsObservation=$report;$finished=$true;break}
				if($report.phase-ceq'Capture'-and$report.captureTag-and!$seen.ContainsKey($report.captureTag)){
					$proof.checkpoints+=,$report;WriteJson (Join-Path $Root ($report.captureTag+'-checkpoint.json')) $report
					foreach($camera in @('CameraA','CameraB')){
					$snapshot=$report[$camera.Substring(0,1).ToLowerInvariant()+$camera.Substring(1)];$stages=if(Active $snapshot){@('raw','processed','composited')}else{@('legacy')};foreach($stage in $stages){$proof.captures+=,(EditorCapture $Root $report $camera $stage)}}
					if($report.captureTag-ceq'baseline'){
						foreach($camera in @('CameraA','CameraB')){$baseline=@($proof.captures|Where-Object{$_.tag-ceq'baseline'-and$_.camera-ceq$camera-and$_.stage-cin@('legacy','raw')});Require ($baseline.Count-eq1) 'Baseline source witness missing.';$witness=AssertChartWitness $Root $baseline[0];WriteJson (Join-Path $Root ($camera+'-chart-witness.json')) $witness}
					}
					# Each completed tag owns a distinct ACK file, so later phases never
					# overwrite a file another Windows reader may still have open.
					$seen[$report.captureTag]=$true;WriteJson (Join-Path $Root ('color-capture-ack-'+$report.captureTag+'.json')) @{runId=$runId;bootId=$boot;tag=$report.captureTag}
				}
			}
			Require (!$owner.process.HasExited) 'Editor exited before fixture completion.';Start-Sleep -Milliseconds 50
		}while($clock.Elapsed.TotalSeconds-lt$TimeoutSeconds)
		Require $finished 'Color fixture/capture deadline expired.'
		$null=Tool 'play.exit';AwaitPlay 'stopped';$proof.status='passed';return $proof
	}catch{$proof.status='failed';$proof.failure=$_.Exception.Message;throw}
	finally {
		$proof.lifecycle=StopOwned $owner
		$clean=(!$proof.lifecycle.forcedTermination-and$proof.lifecycle.exitCode-eq0-and!$proof.lifecycle.cleanupFailure)
		$wasPassed=$proof.status-ceq'passed'
		if($wasPassed-and!$clean){$proof.status='failed';$proof.failure='Editor did not shut down cleanly.'}
		try{WriteJson (Join-Path $Root 'editor-captures.json') $proof}
		catch{if($wasPassed){throw};[Console]::Error.WriteLine('Could not retain cleanup evidence: '+$_.Exception.Message)}
		if($wasPassed){Require $clean 'Editor did not shut down cleanly.'}
	}
}
function RunExport([string]$Root) {
	$null=[IO.Directory]::CreateDirectory($Root);$package=Join-Path $work 'Package'
	$owner=StartOwned $exporter @('--project',$project,'--profile',(Join-Path $project 'BuildProfiles/color.json'),'--output',$package,'--evidence',$Root,'--run-id',$runId) $Root @{}
	$completed=$false
	try{Require ($owner.process.WaitForExit(900000)-and$owner.process.ExitCode-eq0) 'Actual installed export failed.';$completed=$true}
	finally{
		$lifecycle=StopOwned $owner
		try{WriteJson (Join-Path $Root 'process.json') $lifecycle}catch{if($completed){throw};[Console]::Error.WriteLine('Could not retain exporter cleanup evidence: '+$_.Exception.Message)}
		if($completed){Require (!$lifecycle.forcedTermination-and$lifecycle.exitCode-eq0-and!$lifecycle.cleanupFailure) 'Exporter did not settle cleanly.'}
	}
	$launcher=Join-Path $package 'PostProcessColorContract.exe';Require (Test-Path -LiteralPath $launcher -PathType Leaf) 'Exported launcher missing.'
	foreach($file in Get-ChildItem -LiteralPath $package -Recurse -File|Where-Object{$_.Extension-cin@('.exe','.dll','.tepak','.spv')}){$summary.inputs+=,(Pin $file.FullName)}
	return $launcher
}
function RunRuntime([string]$Root,[string]$Launcher) {
	$null=[IO.Directory]::CreateDirectory($Root)
	$envs=@{TERRAENGINE_COLOR_FIXTURE_ROOT=$Root;TERRAENGINE_COLOR_FIXTURE_RUN_ID=$runId;TERRAENGINE_COLOR_FIXTURE_CURVE_ID=$curveId;TERRAENGINE_COLOR_FIXTURE_LUT_ID=$lutId;TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT=(Join-Path $Root 'save-data')}
	$owner=StartOwned $Launcher @('--validate-postprocess-color',$Root) $Root $envs
	try{Require ($owner.process.WaitForExit($TimeoutSeconds*1000)) 'Runtime color seam exceeded deadline.';Require ($owner.process.ExitCode-eq0) 'Runtime native capture seam failed.'}finally{$lifecycle=StopOwned $owner}
	Require (!$lifecycle.forcedTermination-and$lifecycle.exitCode-eq0-and!$lifecycle.cleanupFailure) 'Runtime did not stop cleanly.'
	$path=Join-Path $Root 'runtime-captures.json';Require (Test-Path -LiteralPath $path -PathType Leaf) 'Runtime emitted no real capture evidence.'
	$proof=ReadJson $path;Require ($proof.schemaVersion-eq1-and$proof.runId-ceq$runId-and$proof.status-ceq'passed'-and$proof.orderlyShutdown) 'Runtime capture evidence identity/status mismatch.'
	foreach($checkpoint in $proof.checkpoints){CheckCheckpoint $checkpoint $runId $proof.bootId}
	foreach($row in $proof.captures){
		Require ($row.cameraUUID-ceq$cameraIds[$row.camera]-and$row.width-eq$width-and$row.height-eq$height-and[uint64]$row.frameId-gt0-and[uint64]$row.backendTarget.targetGeneration-gt0) 'Runtime capture native identity/extent absent.'
		if($row.stage-cne'legacy'){Require ([uint64]$row.settingsRevision-eq[uint64]$row.expectedSettingsRevision-and$row.sourceEncoding-cin@('linear','srgb')-and$row.deliveredEncoding-cin@('linear','srgb')) 'Runtime exact revision/transfer missing.'}
	}
	return $proof
}
try {
	Materialize
	foreach($file in @($editor,$exporter,(Join-Path $install 'Editor/TerraEngine.dll'),$PSCommandPath,(Join-Path $fixture 'Scripts/ColorSettingsFixture.cs'))){$summary.inputs+=,(Pin $file)}
	foreach($file in Get-ChildItem -LiteralPath (Join-Path $project 'Assets') -Recurse -File){$summary.inputs+=,(Pin $file.FullName)}
	if($MaterializeOnly){$summary.status='prepared';WriteJson (Join-Path $work 'color-acceptance.json') $summary;return}
	$settingsPath=Join-Path $project 'ProjectSettings/RenderSettings.json';$settingsHash=(Pin $settingsPath).sha256
	$editorRoot=Join-Path $work 'editor';$editorProof=RunEditor $editorRoot;$summary.lanes+=@{mode='editor';evidence=(Join-Path $editorRoot 'editor-captures.json')}
	$scenePath=Join-Path $project 'Assets/Scenes/Startup.tescene';$savedScene=(Pin $scenePath).sha256;$summary.inputs+=,(Pin $scenePath);$summary.inputs+=,(Pin $settingsPath)
	$launcher=RunExport (Join-Path $work 'export')
	$runtimeRoot=Join-Path $work 'runtime';$runtimeProof=RunRuntime $runtimeRoot $launcher;$summary.lanes+=@{mode='export';evidence=(Join-Path $runtimeRoot 'runtime-captures.json')}
	Require ((Pin $scenePath).sha256-ceq$savedScene) 'Play/export modified the saved authored fixture scene.'
	Require ((Pin $settingsPath).sha256-ceq$settingsHash) 'Runtime graphics controls altered authored renderer defaults.'
	$summary.comparisons+=@(ScoreLane $editorRoot $editorProof);$summary.comparisons+=@(ScoreLane $runtimeRoot $runtimeProof)
	# Cross-process comparisons use the same saved authored project, stage, camera,
	# revision-qualified checkpoint and declared transfer. No resized output passes.
	$editorRows=@($editorProof.captures);$runtimeRows=@($runtimeProof.captures)
	foreach($row in $editorRows){$other=@($runtimeRows|Where-Object{$_.tag-ceq$row.tag-and$_.camera-ceq$row.camera-and$_.stage-ceq$row.stage});Require ($other.Count-eq1) 'Export omitted matched stage.'
		Require ($other[0].sourceEncoding-ceq$row.sourceEncoding-and$other[0].deliveredEncoding-ceq$row.deliveredEncoding) 'Editor/export transfer metadata differs.'
		$a=ReadPixels (Join-Path $editorRoot $row.png) $row.deliveredEncoding;$b=ReadPixels (Join-Path $runtimeRoot $other[0].png) $other[0].deliveredEncoding
		$difference=Difference $a $b;Require ($difference-le.012) "Editor/export image parity failed: $($row.tag)/$($row.camera)/$($row.stage) ($difference)."
		$summary.comparisons+=@{name='editor-export-parity';tag=$row.tag;camera=$row.camera;stage=$row.stage;meanAbsoluteLinearError=$difference;tolerance=.012}
	}
	$summary.temporalAdaptationSpeedImageEvidence='not-proven-by-static-chart; native publication checkpoints retained'
	$summary.status='passed-static-color'
}catch{$summary.status='failed';$summary.failure=$_.Exception.Message}
finally{WriteJson (Join-Path $work 'color-acceptance.json') $summary}
if($summary.status-cne'passed-static-color'){throw $summary.failure}
Write-Output (Join-Path $work 'color-acceptance.json')
