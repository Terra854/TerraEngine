<#
Detached fixture generation only. Requires PowerShell 7 and a complete installed
RendererEfficiency materializer/input closure. Never launches the editor.
Usage: pwsh -File MaterializeRuntimeMemorySafetyFixture.ps1 -InstallRoot <Build>
  -WorkRoot <fresh directory> -Scale small [-SelfCheck]
The bootstrap is preserved separately; its RendererEfficiency marker never authorizes
the derivative project. The parent owns play, viewport, readiness, and captures.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$WorkRoot,
	[Parameter(Mandatory)][ValidateSet('small','normal','stress')][string]$Scale,
	[string]$ManifestPath = (Join-Path $PSScriptRoot 'Fixtures/manifest.json'),
	[switch]$SelfCheck
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Message) {
	if (-not $Condition) { throw $Message }
}
function FullPath([string]$Path) { [IO.Path]::GetFullPath($Path) }
function Within([string]$Path, [string]$Root) {
	$Path.Equals($Root, [StringComparison]::OrdinalIgnoreCase) -or
		$Path.StartsWith($Root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}
function NoRedirect([string]$Path) {
	# Validate every existing ancestor before creating anything beneath it.
	$current = FullPath $Path
	while ($current) {
		if (Test-Path -LiteralPath $current) {
			Require (-not ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Fixture paths cannot contain reparse points.'
		}
		$parent = [IO.Path]::GetDirectoryName($current)
		if ($parent -eq $current) { break }
		$current = $parent
	}
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function ReadJson([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable }
function WriteJson([string]$Path, $Value) {
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
	$text = (ConvertTo-Json -InputObject $Value -Depth 100).Replace("`r`n", "`n") + "`n"
	[IO.File]::WriteAllText($Path, $text, [Text.UTF8Encoding]::new($false))
}
function Clone($Value) { ConvertTo-Json -InputObject $Value -Depth 100 -Compress | ConvertFrom-Json -AsHashtable }
function PrepareObjects($Objects) {
	# Remove only known bootstrap lighting pool objects; foundation geometry and
	# its cooked assets remain byte-identical. All inherited illumination/cameras
	# are removed to make the requested active membership unambiguous.
	$result = [Collections.Generic.List[object]]::new()
	foreach ($item in $Objects) {
		if ($item.uuid -match '^6520000000000000[0-9a-fA-F]{16}$') { continue }
		if ($item.Contains('components')) {
			$item.components.Remove('CameraComponent') | Out-Null
			$item.components.Remove('LightComponent') | Out-Null
			if ($item.components.Contains('AnimatorComponent')) { $item.components.AnimatorComponent.playAutomatically = $false }
		}
		if ($item.Contains('children')) { $item.children = @(PrepareObjects $item.children) }
		$result.Add($item)
	}
	$result.ToArray()
}
function Generate([string]$Root, $Manifest) {
	Require (-not (Test-Path -LiteralPath $Root)) 'Each generation root must be fresh and nonexistent.'
	[IO.Directory]::CreateDirectory($Root) | Out-Null
	$bootstrapRoot = Join-Path $Root 'bootstrap'
	$output = & $script:materializer --install-root $script:install --work-root $bootstrapRoot --fixture lighting-dirty --stage P2 2>&1
	Require ($LASTEXITCODE -eq 0) ('Installed bootstrap failed: ' + ($output -join "`n"))
	$bootstrapReceipt = ReadJson (Join-Path $bootstrapRoot 'materialization.json')
	$bootstrapProject = Join-Path $bootstrapRoot 'project'
	$project = Join-Path $Root 'project'
	[IO.Directory]::CreateDirectory($project) | Out-Null
	# Copy a bounded installed input closure and ordinary project configuration.
	# Do not copy RendererEfficiency ownership, camera, or step-controller documents.
	$excluded = @('renderer-efficiency-project.json','renderer-efficiency-camera.json','renderer-efficiency-steps.json')
	foreach ($file in Get-ChildItem -LiteralPath $bootstrapProject -File -Recurse) {
		NoRedirect $file.FullName
		$relative = [IO.Path]::GetRelativePath($bootstrapProject, $file.FullName)
		if ($relative.Replace('\','/') -like 'renderer-efficiency-*' -or $file.Name -in $excluded) { continue }
		$destination = Join-Path $project $relative
		[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
		Copy-Item -LiteralPath $file.FullName -Destination $destination
		Require ((Hash $destination) -ceq (Hash $file.FullName)) 'Copied bootstrap file changed.'
	}
	$scenePath = Join-Path $project 'Assets/Scenes/Scene.tescene'
	$scene = ReadJson $scenePath
	$foundationCameras = @($scene.objects | Where-Object { $_.components.Contains('CameraComponent') })
	Require ($foundationCameras.Count -eq 1) 'Installed foundation must have one canonical camera.'
	$cameraTemplate = Clone $foundationCameras[0]
	$objects = [Collections.Generic.List[object]]::new()
	foreach ($item in @(PrepareObjects $scene.objects)) { $objects.Add($item) }
	$definition = @($Manifest.scales | Where-Object { $_.id -ceq $Scale })
	Require ($definition.Count -eq 1) 'Scale must have exactly one frozen definition.'
	$cameraRows = [Collections.Generic.List[object]]::new()
	$lightRows = [Collections.Generic.List[object]]::new()
	for ($ordinal = 1; $ordinal -le $definition[0].cameraCount; ++$ordinal) {
		$camera = Clone $cameraTemplate
		$camera.uuid = $Manifest.cameraUuidPrefix + $ordinal.ToString('x16')
		$camera.name = "Runtime Memory Camera $ordinal"
		$camera.active = $true
		$camera.children = @()
		$camera.components.CameraComponent.aspectRatio = 1920.0 / 1080.0
		$objects.Add($camera)
		$cameraRows.Add([ordered]@{ ordinal=$ordinal; uuid=$camera.uuid; components=$camera.components; active=$true })
	}
	for ($ordinal = 1; $ordinal -le $definition[0].lightCount; ++$ordinal) {
		$transform = Clone $cameraTemplate.components.Transform
		$transform.WorldPose.position.offsetMetres = @(0,3,0)
		$transform.WorldPose.rotation = @(1,0,0,0)
		$transform.ParentRelativePose.translationMetres = @(0,3,0)
		$transform.ParentRelativePose.rotation = @(1,0,0,0)
		$light = [ordered]@{ uuid=($Manifest.lightUuidPrefix + $ordinal.ToString('x16')); name="Runtime Memory Light $ordinal";
			active=$true; isStatic=$false; tag='Untagged'; layer='Default'; children=@(); components=[ordered]@{
				Transform=$transform; LightComponent=[ordered]@{ type=1; unit=0; color=@(1,1,1); intensity=0.25;
					range=30.0; castsShadows=$false; authoredPriority=(4097-$ordinal); stabilityPreference=0.0 } } }
		$objects.Add($light)
		$lightRows.Add([ordered]@{ ordinal=$ordinal; uuid=$light.uuid; components=$light.components; active=$true })
	}
	$scene.name = "Runtime Memory Safety $Scale"
	$scene.objects = $objects.ToArray()
	WriteJson $scenePath $scene
	WriteJson (Join-Path $project 'runtime-memory-safety-cameras.json') $cameraRows.ToArray()
	WriteJson (Join-Path $project 'runtime-memory-safety-lights.json') $lightRows.ToArray()
	# Verify serialized membership, including the singleton small-camera array.
	# Ordinal order matches the unchanged runtime membership UUID-byte sort.
	$readCameras = @(ReadJson (Join-Path $project 'runtime-memory-safety-cameras.json'))
	$readLights = @(ReadJson (Join-Path $project 'runtime-memory-safety-lights.json'))
	Require ($readCameras.Count -eq $definition[0].cameraCount -and $readLights.Count -eq $definition[0].lightCount) 'Serialized fixture membership count changed.'
	for ($ordinal=1; $ordinal -le $readCameras.Count; ++$ordinal) {
		Require ($readCameras[$ordinal-1].uuid -ceq ($Manifest.cameraUuidPrefix + $ordinal.ToString('x16'))) 'Camera identity/order changed.'
	}
	for ($ordinal=1; $ordinal -le $readLights.Count; ++$ordinal) {
		Require ($readLights[$ordinal-1].uuid -ceq ($Manifest.lightUuidPrefix + $ordinal.ToString('x16'))) 'Light identity/order changed.'
	}
	$installedWorkloads = ReadJson (Join-Path $script:install 'Tests/Fixtures/RendererEfficiency/workloads.json')
	$fixedCamera = @($installedWorkloads.cameras | Where-Object { $_.id -ceq 'scene-fixed' })
	Require ($fixedCamera.Count -eq 1) 'Installed fixed camera is missing.'
	WriteJson (Join-Path $project 'runtime-memory-safety-request.json') ([ordered]@{
		seed=$Manifest.seed; scale=$Scale; requestedExtent=$Manifest.requestedExtent; requestedCamera=$fixedCamera[0];
		expectedAuthoredCameras=$definition[0].cameraCount; expectedAuthoredLights=$definition[0].lightCount;
		observedViews=$null; observedExtent=$null; readinessStatus='Unverified';
		warning='This request does not resize views, suppress Game outputs, or observe captured collection order.' })
	$closure = [Collections.Generic.List[object]]::new()
	foreach ($row in $bootstrapReceipt.inputClosure) {
		$installedPath = FullPath (Join-Path $script:install $row.installedPath)
		Require (Within $installedPath $script:install) 'Input closure escaped InstallRoot.'
		Require (Within (FullPath (Join-Path $project $row.projectPath)) $project) 'Input closure escaped derivative project.'
		Require ((Hash $installedPath) -ceq $row.sha256) 'Installed source closure changed.'
		Require ((Hash (Join-Path $project $row.projectPath)) -ceq $row.sha256) 'Derivative input closure changed.'
		$closure.Add($row)
	}
	$generated = @('Assets/Scenes/Scene.tescene','ProjectSettings/RenderSettings.json','runtime-memory-safety-cameras.json',
		'runtime-memory-safety-lights.json','runtime-memory-safety-request.json','project.json','game.json','Library/LastScene.txt')
	$hashes = [ordered]@{}
	foreach ($relative in $generated) { $hashes[$relative] = Hash (Join-Path $project $relative) }
	$receipt = [ordered]@{ schemaVersion=1; kind='RuntimeMemorySafetyValidationProject'; fixtureId="runtime-memory-safety-$Scale";
		seed=$Manifest.seed; scale=$Scale; fixtureManifestSha256=$script:manifestHash; generatorSha256=$script:generatorHash;
		bootstrapMaterializerSha256=$script:materializerHash; bootstrapManifestSha256=(Hash (Join-Path $script:install 'Tests/Fixtures/RendererEfficiency/workloads.json'));
		inputClosure=$closure.ToArray(); generatedHashes=$hashes; expectedCameraCount=$definition[0].cameraCount;
		expectedLightCount=$definition[0].lightCount; requestedExtent=$Manifest.requestedExtent;
		status='AuthoredAwaitingRuntimeReadiness'; referenceInternalCollectionProof='UnavailableThroughCommonAutomation';
		projectRoot=$project; bootstrapReceiptPath=(Join-Path $bootstrapRoot 'materialization.json') }
	WriteJson (Join-Path $project 'runtime-memory-safety-project.json') $receipt
	WriteJson (Join-Path $Root 'materialization.json') $receipt
	Require ((Hash $script:materializer) -ceq $script:materializerHash) 'Installed materializer changed during generation.'
	Require ((Hash $manifestFile) -ceq $script:manifestHash -and (Hash $PSCommandPath) -ceq $script:generatorHash) 'Generator/manifest changed during generation.'
	return $receipt
}

$script:install = FullPath $InstallRoot
$work = FullPath $WorkRoot
$manifestFile = FullPath $ManifestPath
foreach ($path in @($script:install,$work,$manifestFile)) { NoRedirect $path }
Require (Test-Path -LiteralPath $script:install -PathType Container) 'InstallRoot does not exist.'
Require (-not (Within $work $script:install) -and -not (Within $script:install $work)) 'WorkRoot and InstallRoot cannot overlap.'
Require (-not (Test-Path -LiteralPath $work)) 'WorkRoot must not exist.'
$script:materializer = Join-Path $script:install 'Tests/Tools/RendererEfficiency/TerraEngine-RendererEfficiencyFixtureMaterializer.exe'
Require (Test-Path -LiteralPath $script:materializer -PathType Leaf) 'Installed materializer is missing.'
NoRedirect $script:materializer
$manifest = ReadJson $manifestFile
Require ($manifest.schemaVersion -eq 1 -and $manifest.manifestId -ceq 'runtime-memory-safety-fixtures-v1' -and $manifest.seed -eq 66007) 'Frozen fixture identity changed.'
$expectedScales = @(@('small',1,16),@('normal',4,256),@('stress',16,4096))
Require ($manifest.scales.Count -eq 3) 'Frozen scale count changed.'
for ($index=0; $index -lt 3; ++$index) {
	Require ($manifest.scales[$index].id -ceq $expectedScales[$index][0] -and
		$manifest.scales[$index].cameraCount -eq $expectedScales[$index][1] -and
		$manifest.scales[$index].lightCount -eq $expectedScales[$index][2]) 'Frozen scale membership changed.'
}
Require ($manifest.cameraUuidPrefix -ceq '066c000000000000' -and $manifest.lightUuidPrefix -ceq '066d000000000000') 'Frozen UUID namespace changed.'
Require ($manifest.requestedExtent.width -eq 1920 -and $manifest.requestedExtent.height -eq 1080) 'Frozen requested extent changed.'
Require ($manifest.bootstrapFixture -ceq 'lighting-dirty' -and $manifest.bootstrapStage -ceq 'P2' -and
	$manifest.warmupFrames -eq 60 -and $manifest.measuredFrames -eq 300) 'Frozen bootstrap/window changed.'
Require ($manifest.lightPolicy.type -eq 1 -and $manifest.lightPolicy.unit -eq 0 -and
	$manifest.lightPolicy.intensity -eq 0.25 -and $manifest.lightPolicy.range -eq 30 -and
	$manifest.lightPolicy.castsShadows -eq $false -and $manifest.lightPolicy.priorityBase -eq 4097) 'Frozen light recipe changed.'
Require ($manifest.imageNormalizedChannelError -eq (2.0/255.0) -and
	$manifest.allowExcludedImageRegions -eq $false -and $manifest.missingEvidenceResult -ceq 'Unverified') 'Frozen evidence policy changed.'
$script:manifestHash = Hash $manifestFile
$script:generatorHash = Hash $PSCommandPath
$script:materializerHash = Hash $script:materializer
if ($SelfCheck) {
	[IO.Directory]::CreateDirectory($work) | Out-Null
	$first = Generate (Join-Path $work 'first') $manifest
	$second = Generate (Join-Path $work 'second') $manifest
	Require (($first.generatedHashes | ConvertTo-Json -Compress) -ceq ($second.generatedHashes | ConvertTo-Json -Compress)) 'Generation differs across fresh roots.'
	Require (($first.inputClosure | ConvertTo-Json -Depth 10 -Compress) -ceq ($second.inputClosure | ConvertTo-Json -Depth 10 -Compress)) 'Input closures differ across fresh roots.'
	$result = [ordered]@{ status='DeterministicGenerationPassed'; scale=$Scale; generatorSha256=$script:generatorHash;
		fixtureManifestSha256=$script:manifestHash; generatedHashes=$first.generatedHashes; firstProjectRoot=$first.projectRoot;
		secondProjectRoot=$second.projectRoot; runtimeReadiness='Unverified'; referenceCollectionOrder='Unverified' }
	WriteJson (Join-Path $work 'self-check.json') $result
	$result | ConvertTo-Json -Depth 20
} else { Generate $work $manifest | ConvertTo-Json -Depth 30 }
