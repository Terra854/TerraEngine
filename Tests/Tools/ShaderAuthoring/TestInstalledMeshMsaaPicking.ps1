# This fixture verifies the actual RGBA32_UINT pick readback on a mesh MSAA
# Scene View. It starts and closes a disposable editor and project.
param(
	[string]$InstallRoot = 'out/build/RelWithDebInfo/Build',
	[string]$WorkRoot = 'out/validation/mesh-msaa',
	[uint16]$Port = 18864,
	[switch]$VulkanValidation,
	[switch]$IncludePostProcessModes
)
$ErrorActionPreference = 'Stop'
$editor = (Resolve-Path (Join-Path $InstallRoot 'Editor')).Path
$work = [IO.Path]::GetFullPath((Join-Path $WorkRoot ([guid]::NewGuid().ToString('N'))))
$project = Join-Path $work 'Picking'
$assets = Join-Path $project 'Assets'
$scenes = Join-Path $assets 'Scenes'
$settings = Join-Path $project 'ProjectSettings'
New-Item -Path $scenes,$settings -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $editor 'Project/Templates/UserSettings') (Join-Path $project 'UserSettings') -Recurse
Copy-Item 'Tests/Fixtures/ExportRuntimeBaseline/ProjectSettings/ProjectSettings.terra' -Destination $settings
Set-Content "$project/game.json" '{"name":"Mesh MSAA Picking","version":"1.0.0","startupScene":"Scenes/Startup.tescene"}'

function Write-Json($Path, $Value) {
	ConvertTo-Json -InputObject $Value -Depth 16 -Compress | Set-Content -LiteralPath $Path
}
function Asset-Metadata($Name, $Id, $Importer, $Cooked) {
	Write-Json "$assets/$Name.meta" @{guid=$Id; importerName=$Importer; lastModified=0; sourcePath=$Name; compiledPath=$Cooked; settings=@{}}
}
$meshId = 'a0000000000000000000000000000001'
$defaultMaterial = 'a0000000000000000000000000000002'
$customMaterial = 'a0000000000000000000000000000003'
$collection = 'a0000000000000000000000000000004'
$stockObject = 'a0000000000000000000000000000005'
$customObject = 'a0000000000000000000000000000006'
@"
v -0.5 -0.5 0
v 0.5 -0.5 0
v 0.5 0.5 0
v -0.5 0.5 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
f 1/1/1 2/2/1 3/3/1
f 1/1/1 3/3/1 4/4/1
"@ | Set-Content "$assets/Quad.obj"
Asset-Metadata 'Quad.obj' $meshId 'MeshImporter' 'Quad.temf'
function Surface-Material($Name, $Id, $Shader) {
	# TMAT v1 with no textures or extra parameters. The asset sidecar owns its GUID.
	$stream = [IO.BinaryWriter]::new([IO.File]::Create("$assets/$Name"))
	try {
		$stream.Write([uint32]0x54414d54)
		$stream.Write([uint16]1)
		$stream.Write([uint16]100)
		$stream.Write([uint32]1)
		$stream.Write([uint32]0)
		$stream.Write([uint32]0)
		$stream.Write([Convert]::FromHexString($Shader))
		foreach ($value in @(0.3,0.8,0.4,1, 0,0,0, 0,1,1,1,0.5)) { $stream.Write([single]$value) }
		foreach ($value in @(0,0,100,100)) { $stream.Write([uint32]$value) }
	} finally { $stream.Dispose() }
	Asset-Metadata $Name $Id 'MaterialImporter' $Name
}
Surface-Material 'Default.tmat' $defaultMaterial ('0' * 32)
Surface-Material 'Custom.tmat' $customMaterial $collection
$templates = Join-Path $editor 'Project/Templates/ShaderSources/Default'
$meshSource = Get-Content "$templates/Default.mesh.hlsl" -Raw
$originalPosition = 'outVertex.position = mul(SceneUniforms.viewProj, worldPosition);'
if (!$meshSource.Contains($originalPosition)) { throw 'The mesh shader test edit needs updating.' }
$meshSource.Replace($originalPosition, "$originalPosition outVertex.position.y += 0.3 * outVertex.position.w;") | Set-Content "$assets/Custom.mesh.hlsl"
$fragment = Get-Content "$templates/Default.frag.hlsl" -Raw
$originalColor = 'TERRAENGINE_CONVENTIONAL_RETURN(shaded, stablePrimitiveId);'
if (!$fragment.Contains($originalColor)) { throw 'The fragment shader test edit needs updating.' }
$fragment.Replace($originalColor, 'TERRAENGINE_CONVENTIONAL_RETURN(shaded.bgra, stablePrimitiveId);') | Set-Content "$assets/Custom.frag.hlsl"
Write-Json "$assets/Custom.teshadercollection" @{task='';mesh='Custom.mesh.hlsl';fragment='Custom.frag.hlsl'}
Asset-Metadata 'Custom.teshadercollection' $collection 'ShaderCollectionImporter' 'Custom.teshadercollection'
$cell = @{x='0';y='0';z='0'}
function Fixture-Object($Id, $Name, $X, $Material) {
	$position = @([double]$X,0,0)
	$rotation = @(1,0,0,0)
	$scale = @(1,1,1)
	return @{uuid=$Id;name=$Name;components=@{
		Transform=@{
			WorldPose=@{position=@{cell=$cell;offsetMetres=$position};rotation=$rotation;scale=$scale}
			ParentRelativePose=@{translationMetres=$position;rotation=$rotation;scale=$scale;transformSpaceId=0}
		}
		MeshRenderComponent=@{meshId=$meshId;materialId=$Material}
	}}
}
$camera = Fixture-Object 'a0000000000000000000000000000007' 'Fixture camera' 0 $defaultMaterial
$camera.components.Remove('MeshRenderComponent')
$camera.components.Transform.WorldPose.position.offsetMetres = @(0,0,4)
$camera.components.Transform.ParentRelativePose.translationMetres = @(0,0,4)
$camera.components.CameraComponent = @{verticalFovDegrees=60;nearPlane=0.1;farPlane=100}
Write-Json "$scenes/Startup.tescene" @{format='TerraSceneUnboundedV1';name='Picking';objects=@(
	(Fixture-Object $stockObject 'Built-in surface' -1 $defaultMaterial),
	(Fixture-Object $customObject 'Customized surface' 1 $customMaterial),
	$camera
)}
$prefs = Join-Path $work 'preferences.json'
Write-Json $prefs @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$Port
	rendererDiagnosticsPreferencesInitialized=$true;vulkanValidationLayers=[bool]$VulkanValidation;renderDebugDiagnostics=$true}
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) { throw "Port $Port is in use." }
$stdout = Join-Path $work 'editor.stdout.log'
$stderr = Join-Path $work 'editor.stderr.log'
$p = Start-Process -FilePath "$editor/TerraEngine-Editor.exe" -WorkingDirectory $editor -ArgumentList ('--project "'+$project+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Environment @{
	TERRAENGINE_EDITOR_PREFS_SESSION_PATH=$prefs; TERRAENGINE_SUPPRESS_PROJECT_RECENTS='1'
}
function Call-Editor($Name, $Arguments=@{}) {
	$request = @{jsonrpc='2.0';id=[guid]::NewGuid().ToString('N');method='tools/call';params=@{name=$Name;arguments=$Arguments}}
	$reply = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/mcp" -Method Post -ContentType 'application/json' -Body (ConvertTo-Json $request -Depth 20 -Compress) -TimeoutSec 10
	$envelope = ($reply.result.content | Where-Object type -eq 'text').text | ConvertFrom-Json
	if (!$envelope.ok) { throw "$Name failed. $($envelope.message) $($reply.error.message)" }
	return $envelope.data
}
function Wait-For($Name, [scriptblock]$Check) {
	$clock = [Diagnostics.Stopwatch]::StartNew()
	$failure = ''
	while ($clock.Elapsed.TotalSeconds -lt 35) {
		if ($p.HasExited) { throw "Editor exited during $Name." }
		try { $value = & $Check; if ($value) { return $value } }
		catch { $failure = $_.Exception.Message }
		Start-Sleep -Milliseconds 150
	}
	throw "Timed out waiting for $Name. $failure"
}
try {
	$null = Wait-For 'the owned editor listener' {
		(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue).OwningProcess -eq $p.Id
	}
	$null = Wait-For 'the cooked shaders and open project' {
		$state = Call-Editor 'project.state'
		$state.activeScenePath -and !$state.assetImportsActive -and
			(Select-String -LiteralPath $stdout -Pattern 'Finalized load for .*Custom.teshadercollection' -Quiet)
	}
	$null = Call-Editor 'scene_view.camera.set' @{position=@{cell=$cell;offsetMetres=@(0,0,4)};focus=@{cell=$cell;offsetMetres=@(0,0,0)}}
	$modes = if ($IncludePostProcessModes) { @(0,1,2,4,5) } else { @(0,4) }
	foreach ($mode in $modes) {
		$state = Wait-For 'the registered Scene View' { $s = Call-Editor 'scene_view.graphics.get'; if ($s.view.lifetimeGeneration -ne '0') { $s } }
		$request = Call-Editor 'scene_view.graphics.request' @{lifecycle=$state.lifecycle;view=$state.view;baseRevision=$state.revision;values=@(@{id=64;value=$mode})}
		$applied = Wait-For "applied mode $mode" {
			$s = Call-Editor 'scene_view.graphics.get'
			$r = (Call-Editor 'scene_view.graphics.receipt' @{graphicsRequestId=$request.receipt.graphicsRequestId}).receipt
			if ($r.reason -ne 0) { throw $r.detail }
			if ([uint64]$r.appliedFrameId -gt 0 -and ($s.effective.values | Where-Object id -eq 64).value -eq $mode) { $r }
		}
		$capture = Wait-For 'a native Scene View image' {
			$frame = Call-Editor 'scene_view.capture' @{freshness='latestCompleted'}
			if ($frame.backendReadback -and [uint64]$frame.frameId -ge [uint64]$applied.appliedFrameId) { $frame }
		}
		$size = @($capture.backendTarget.sourceWidth,$capture.backendTarget.sourceHeight)
		if ($size[0] -lt 100 -or $size[1] -lt 100) { throw "The native viewport is too small: $size" }
		# Fixed camera and two quads. The custom mesh raises its clip-space y
		# by 0.3. Vulkan's downward framebuffer y puts it lower on screen.
		$offset = $size[1] / ($size[0] * 8 * [Math]::Tan([Math]::PI/6))
		$checks = @(
			@{x=(0.5-$offset);y=0.5;id=$stockObject},
			@{x=(0.5+$offset);y=0.65;id=$customObject}
		)
		foreach ($check in $checks) {
			$pixel = @([int]($size[0]*$check.x),[int]($size[1]*$check.y))
			$null = Call-Editor 'renderer.virtualGeometry.authoritativePick' @{viewType='Scene';pixel=$pixel;request=$true}
			$pick = Wait-For "GPU pick at $pixel in mode $mode" {
				$result = (Call-Editor 'renderer.virtualGeometry.authoritativePick' @{viewType='Scene';pixel=$pixel;request=$false}).authoritative
				if ($result.status -eq 'Ready' -and [uint64]$result.pick.frameId -ge [uint64]$applied.appliedFrameId) { $result.pick }
			}
			if ($pick.objectId -ne $check.id) { throw "Wrong object from GPU pick at $pixel in mode $mode. Got $($pick.objectId)." }
			Write-Output "Mode=$mode pixel=$pixel GPU picked $($pick.objectId)"
		}
	}
} finally {
	if (!$p.HasExited) {
		$null = $p.CloseMainWindow()
		if (!$p.WaitForExit(6000)) { Stop-Process -Id $p.Id }
	}
	Write-Output "Smoke evidence: $work"
}
if ($VulkanValidation -and (Select-String -LiteralPath $stdout,$stderr -Pattern 'Validation Error|VUID-' -Quiet)) {
	throw 'Vulkan reported a validation error. See the saved editor logs.'
}



