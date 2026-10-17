<#
Creates disposable, closed replay projects using maintained authored sources and
the installed editor's actual importer. Requires PowerShell 7, a complete testing
install and a Vulkan adapter. This utility never qualifies a replay/GPU profile:
the matching-header collector must still observe the requested production route.
Usage: pwsh -NoProfile -File PrepareFrameReplayProjects.ps1 -InstallRoot Build
  -WorkRoot out/validation/runtime-contract-quality/replay-projects-01
WorkRoot must not exist. All logs and failed partial projects remain for diagnosis.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$WorkRoot,
	[ValidateRange(1024,65535)][int]$McpPort = 43291,
	[ValidateRange(30,600)][int]$StartupTimeoutSeconds = 180,
	[ValidateRange(60,1800)][int]$ProfileTimeoutSeconds = 600
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
$utf8 = [Text.UTF8Encoding]::new($false)
function Require([bool]$Condition, [string]$Message) { if(!$Condition) { throw $Message } }
function Full([string]$Path) { [IO.Path]::GetFullPath($Path).TrimEnd('\','/') }
function Within([string]$Child, [string]$Root) {
	$Child.Equals($Root, [StringComparison]::OrdinalIgnoreCase) -or $Child.StartsWith($Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}
function NoRedirect([string]$Path) {
	$current = [IO.Path]::GetFullPath($Path)
	while($current) {
		if(Test-Path -LiteralPath $current) { Require (!((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) "Redirected path: $current" }
		$current = [IO.Path]::GetDirectoryName($current)
	}
}
function Hash([string]$Path) { NoRedirect $Path; (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Pin([string]$Path) {
	$file = Get-Item -LiteralPath $Path
	Require (!$file.PSIsContainer) "Expected file: $Path"
	$bytes = $file.Length; $hash = Hash $Path
	Require ((Get-Item -LiteralPath $Path).Length -eq $bytes -and (Hash $Path) -ceq $hash) "File changed while pinning: $Path"
	[ordered]@{ path = Full $Path; bytes = $bytes; sha256 = $hash }
}
function ReadJson([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable }
function WriteJson([string]$Path, $Value) {
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
	[IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 100).Replace("`r`n", "`n") + "`n"), $utf8)
}
function Clone($Value) { ConvertTo-Json -InputObject $Value -Depth 100 -Compress | ConvertFrom-Json -AsHashtable }
function CopyChecked([string]$Source, [string]$Target) {
	$before = Hash $Source
	Require (!(Test-Path -LiteralPath $Target)) "Copy target already exists: $Target"
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Target)) | Out-Null
	Copy-Item -LiteralPath $Source -Destination $Target
	Require ((Hash $Source) -ceq $before -and (Hash $Target) -ceq $before) "Copy bytes changed: $Source"
}
function SourceHash([string]$Path) {
	# Match the maintained native importer's unsigned FNV-1a and unpadded hex.
	# BigInteger makes modulo-2^64 explicit without a compiler or global package.
	[System.Numerics.BigInteger]$value = 1469598103934665603
	[System.Numerics.BigInteger]$mask = [System.Numerics.BigInteger]::Pow(2,64) - 1
	foreach($byte in [IO.File]::ReadAllBytes($Path)) { $value = (($value -bxor $byte) * 1099511628211) -band $mask }
	([uint64]$value).ToString('x')
}
function Rpc([string]$Method, $Arguments = @{}) {
	Require (!$script:editor.HasExited) 'Owned editor exited during preparation.'
	$remaining = [int][Math]::Floor($ProfileTimeoutSeconds - $script:clock.Elapsed.TotalSeconds)
	Require ($remaining -gt 0) 'Profile watchdog expired.'
	$script:rpcId++
	$request = @{jsonrpc='2.0'; id=$script:rpcId; method=$Method; params=$Arguments}
	try {
		$response = (Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request | ConvertTo-Json -Depth 100 -Compress) -TimeoutSec ([Math]::Min(120,$remaining))).Content | ConvertFrom-Json -AsHashtable
		$record = @{request=$request; response=$response}
	} catch {
		$record = @{request=$request; error=$_.Exception.Message}
		[IO.File]::AppendAllText($script:rpcLog, (($record | ConvertTo-Json -Depth 100 -Compress) + "`n"), $utf8)
		throw
	}
	[IO.File]::AppendAllText($script:rpcLog, (($record | ConvertTo-Json -Depth 100 -Compress) + "`n"), $utf8)
	Require (!$response.ContainsKey('error')) "MCP protocol failure: $Method"
	$response
}
function Tool([string]$Name, $Arguments = @{}) {
	$response = Rpc 'tools/call' @{name=$Name; arguments=$Arguments}
	Require (!$response.result.ContainsKey('isError') -or !$response.result.isError) "MCP tool error: $Name"
	$text = @($response.result.content | Where-Object { $_.type -eq 'text' })
	Require ($text.Count -eq 1) "Missing unique tool envelope: $Name"
	$envelope = $text[0].text | ConvertFrom-Json -AsHashtable
	Require ($envelope.ok -eq $true) "Tool rejected: $Name $($text[0].text)"
	$envelope.data
}
function OwnedState([string]$Project) {
	$state = Tool 'project.state'
	Require ($state.hasProject -and (Full $state.projectRoot) -ieq $Project) 'Owned project identity changed.'
	$state
}
function WaitSettled([string]$Project) {
	# Two distinct clean/import-idle observations match ordinary close readiness.
	$first = $null
	while($script:clock.Elapsed.TotalSeconds -lt $ProfileTimeoutSeconds) {
		$state = OwnedState $Project
		Require ($state.assetImportsActive -is [bool] -and $state.sceneDirty -is [bool]) 'Import/dirty readiness fields unavailable.'
		if(!$state.assetImportsActive -and !$state.sceneDirty) {
			if($null -eq $first) { $first = $script:clock.ElapsedMilliseconds }
			elseif($script:clock.ElapsedMilliseconds - $first -ge 100) { return $state }
		} else { $first = $null }
		Start-Sleep -Milliseconds 100
	}
	throw 'Profile watchdog expired waiting for settled imports and scene.'
}
function ImportReal([string]$Project, [string]$Source) {
	$sourceFile = Join-Path $Project ('Assets/' + $Source)
	$before = Hash $sourceFile
	$result = Tool 'asset.import' @{path=$Source; force=$true}
	Require ($result.ContainsKey('importResult') -and $null -ne $result.importResult) "Importer omitted actual receipt: $Source"
	$receipt = $result.importResult
	Require ($receipt.status -ceq 'Succeeded') "Import did not succeed: $Source"
	Require ((Hash $sourceFile) -ceq $before -and $receipt.package.sourceHash -ceq (SourceHash $sourceFile)) "Import receipt describes different source bytes: $Source"
	$meta = ReadJson ($sourceFile + '.meta')
	Require ($receipt.packageGuid -ceq $meta.guid) "Import package GUID mismatch: $Source"
	$disk = ReadJson ($sourceFile + '.import.json')
	Require (($disk | ConvertTo-Json -Depth 100 -Compress) -ceq ($receipt | ConvertTo-Json -Depth 100 -Compress)) "MCP/disk import receipts differ: $Source"
	$receipt
}
function SetProperty([string]$Object, [string]$Component, [string]$Property, $Value) {
	$null = Tool 'component.set_property' @{object=$Object; component=$Component; property=$Property; value=$Value}
	$read = Tool 'component.get' @{object=$Object; component=$Component}
	Require ($read.properties[$Property] -eq $Value) "Rebinding was not visible: $Object/$Property"
}
function Launch([string]$Project, [string]$Evidence) {
	$script:clock = [Diagnostics.Stopwatch]::StartNew(); $script:rpcId = 0
	$script:stdout=$null; $script:stderr=$null
	$script:rpcLog = Join-Path $Evidence 'mcp.jsonl'
	$prefs = Join-Path $Evidence 'editor-preferences.json'
	WriteJson $prefs @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;vulkanValidationLayers=$false;renderDebugDiagnostics=$false;rendererDiagnosticsPreferencesInitialized=$true}
	$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$McpPort)
	try { $listener.Start() } finally { $listener.Stop() }
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $script:editorPath; $start.WorkingDirectory = Split-Path $script:editorPath
	$start.ArgumentList.Add('--project'); $start.ArgumentList.Add($Project)
	$start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
	$start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
	$start.Environment['TERRAENGINE_EDITOR_PREFS_SESSION_PATH']=$prefs
	$start.Environment['TERRAENGINE_SUPPRESS_PROJECT_RECENTS']='1'
	$script:editor = [Diagnostics.Process]::Start($start)
	$script:stdout = $script:editor.StandardOutput.ReadToEndAsync(); $script:stderr = $script:editor.StandardError.ReadToEndAsync()
	$last = ''
	while($script:clock.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds) {
		Require (!$script:editor.HasExited) 'Editor exited before startup readiness.'
		try {
			$hello = Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='frame-replay-project-preparation';version='1'}}
			Require ($hello.result.serverInfo.name -ceq 'TerraEngine Editor') 'Unexpected MCP server.'
			$listeners = @(Get-NetTCPConnection -LocalPort $McpPort -State Listen)
			Require ($listeners.Count -gt 0 -and @($listeners | Where-Object {$_.OwningProcess -ne $script:editor.Id}).Count -eq 0) 'MCP listener is not owned by launched editor.'
			$state = OwnedState $Project
			if($state.activeScene) { return }
		} catch { $last = $_.Exception.Message }
		Start-Sleep -Milliseconds 200
	}
	throw "Editor startup deadline expired: $last"
}
function CloseOwned([string]$Project) {
	$null = Tool 'scene.save'
	$state = WaitSettled $Project
	$accepted = $script:editor.CloseMainWindow()
	$exited = $accepted -and $script:editor.WaitForExit(15000)
	Require ($accepted -and $exited) 'Owned editor did not close normally.'
	Require ($script:editor.ExitCode -eq 0) 'Owned editor returned nonzero exit code.'
	[ordered]@{normalClose=$true;exitCode=$script:editor.ExitCode;processId=$script:editor.Id;assetImportsActive=$state.assetImportsActive;sceneDirty=$state.sceneDirty;observations=2;minimumSeparationMilliseconds=100}
}
function FinishProcess([string]$Evidence) {
	# A failed/expired profile never becomes qualified. Kill only our exact process;
	# ordinary successful profiles always pass CloseOwned first.
	if($null -eq $script:editor) { return }
	if(!$script:editor.HasExited) { $script:editor.Kill(); $script:editor.WaitForExit(15000) | Out-Null }
	if($script:editor.HasExited -and $null -ne $script:stdout -and $null -ne $script:stderr) {
		[IO.File]::WriteAllText((Join-Path $Evidence 'stdout.log'),$script:stdout.GetAwaiter().GetResult(),$utf8)
		[IO.File]::WriteAllText((Join-Path $Evidence 'stderr.log'),$script:stderr.GetAwaiter().GetResult(),$utf8)
	}
	$script:editor.Dispose(); $script:editor=$null
}
function AuthorActor($Template, [int]$Ordinal, [bool]$Compact) {
	$actor = Clone $Template
	$actor.uuid = '068a000000000000' + $Ordinal.ToString('x16'); $actor.name = "Replay Actor $Ordinal"
	# The imported actor has exactly one body. Keep authored UUIDs distinct across
	# CPU and visual actors; runtime instance handles are never fabricated here.
	Require ($actor.children.Count -eq 1) 'Maintained hero topology changed.'
	$actor.children[0].uuid = '068b000000000000' + $Ordinal.ToString('x16')
	$actor.children[0].name = "Replay Body $Ordinal"
	$animator = $actor.components.AnimatorComponent
	$animator.requestedMode = $(if($Compact) {1} else {3})
	$animator.playAutomatically=$true; $animator.rootMotionPolicy=0; $animator.eventPolicy=2
	$animator.updatePolicy=0; $animator.cullingPolicy=0
	$actor
}
function AuthorStaticGrids($Template, [int]$Count) {
	# Keep the maintained first target and give each additional real scene object
	# a distinct UUID. Mixed needs more than16 renderables so the production
	# GeometryRoutePrepare policy chooses an actual worker without metadata edits.
	Require ($Count -ge 1 -and $Template.children.Count -eq 0) 'Static grid template shape changed.'
	for($ordinal=1; $ordinal -le $Count; $ordinal++) {
		$instance=Clone $Template
		if($ordinal -gt 1) {
			$instance.uuid='068d000000000000'+$ordinal.ToString('x16')
			$instance.name="Replay Static Grid $ordinal"
		}
		$instance
	}
}
function GetAuthoredObjectFacts($Objects) {
	# Inspect the actual detached JSON tree before launch and again after save.
	# These are input counts; the collector still must prove native worker dispatch.
	$ids=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$facts=[ordered]@{objectCount=0;meshRenderComponentCount=0;objectIds=@()}
	$visit={param($Entries)
		foreach($entry in $Entries) {
			Require ($entry.uuid -cmatch '^[0-9a-f]{32}$' -and $entry.uuid -cne ('0'*32)) 'Authored object UUID is invalid.'
			Require ($ids.Add($entry.uuid)) "Duplicate authored object UUID: $($entry.uuid)"
			$facts.objectCount++
			if($entry.components.ContainsKey('MeshRenderComponent')) {
				Require ($entry.components.ContainsKey('Transform')) 'Renderable input lacks Transform.'
				Require ((!$entry.ContainsKey('active') -or $entry.active) -and (!$entry.components.MeshRenderComponent.ContainsKey('active') -or $entry.components.MeshRenderComponent.active)) 'Renderable authored input is inactive.'
				$facts.meshRenderComponentCount++
			}
			if($entry.ContainsKey('children')) { & $visit $entry.children }
		}
	}
	& $visit $Objects
	$facts.objectIds=@($ids | Sort-Object)
	$facts
}

$script:editor = $null
$install = Full $InstallRoot; $work = Full $WorkRoot
NoRedirect $install; NoRedirect $work
Require (!(Within $work $install) -and !(Within $install $work)) 'InstallRoot and WorkRoot overlap.'
Require (!(Test-Path -LiteralPath $work)) 'WorkRoot must be fresh and nonexistent.'
$materializer = Join-Path $install 'Tests/Tools/RendererEfficiency/TerraEngine-RendererEfficiencyFixtureMaterializer.exe'
$script:editorPath = Join-Path $install 'Editor/TerraEngine-Editor.exe'
foreach($file in @($materializer,$script:editorPath)) { Require (Test-Path -LiteralPath $file -PathType Leaf) "Missing installed prerequisite: $file"; NoRedirect $file }
$inputPins = @((Pin $materializer),(Pin $script:editorPath),(Pin $PSCommandPath))
[IO.Directory]::CreateDirectory($work) | Out-Null
$summary = [ordered]@{schemaVersion='frame-replay-project-preparation-v1';status='Failed';inputs=$inputPins;profiles=@();profileQualification='Requires actual matching-header collector observations'}
try {
	# P2 provides the canonical hero scene/cooked closure. P1 provides the maintained
	# static64-grid source; generated sources are cooked by the actual editor below.
	foreach($bootstrap in @(@{name='hero';fixture='lighting-dirty';stage='P2'},@{name='grid';fixture='shadow-invalidation';stage='P1'})) {
		$root = Join-Path $work ('bootstrap/' + $bootstrap.name)
		$log = & $materializer --install-root $install --work-root $root --fixture $bootstrap.fixture --stage $bootstrap.stage 2>&1
		$code = $LASTEXITCODE
		[IO.File]::WriteAllText((Join-Path $work ($bootstrap.name + '-materializer.log')),($log | Out-String),$utf8)
		Require ($code -eq 0) "Maintained materializer failed: $($bootstrap.name)"
		$summary.inputs+=,(Pin (Join-Path $root 'materialization.json'))
	}
	$base = Join-Path $work 'bootstrap/hero/project'
	$baseScene = ReadJson (Join-Path $base 'Assets/Scenes/Scene.tescene')
	$cameras = @($baseScene.objects | Where-Object { $_.components.ContainsKey('CameraComponent') })
	$actors = @($baseScene.objects | Where-Object { $_.components.ContainsKey('AnimatorComponent') })
	Require ($cameras.Count -eq 1 -and $actors.Count -eq 1) 'Maintained foundation camera/actor shape changed.'
	foreach($definition in @(
		@{name='DirectComplete';profile='DirectComplete';variant='Default';cpu=1;compact=0;cameras=1;grid=$false;gridInstances=0},
		@{name='VirtualCompact';profile='VirtualCompact';variant='Default';cpu=0;compact=1;cameras=1;grid=$true;gridInstances=1},
		@{name='Mixed';profile='Mixed';variant='Default';cpu=1;compact=1;cameras=1;grid=$true;gridInstances=17},
		@{name='NoOutput';profile='Mixed';variant='NoOutput';cpu=1;compact=0;cameras=0;grid=$false;gridInstances=0},
		@{name='MultiOutput';profile='Mixed';variant='MultiOutput';cpu=1;compact=1;cameras=2;grid=$true;gridInstances=1}
	)) {
		$profileRoot = Join-Path $work ('profiles/' + $definition.name)
		$project = Join-Path $profileRoot 'project'; $evidence = Join-Path $profileRoot 'preparation'
		[IO.Directory]::CreateDirectory($evidence) | Out-Null
		foreach($file in Get-ChildItem -LiteralPath $base -Recurse -File | Sort-Object FullName) {
			$relative = [IO.Path]::GetRelativePath($base,$file.FullName)
			# RendererEfficiency authority belongs to its unchanged workload only.
			if($relative.Replace('\','/').StartsWith('renderer-efficiency-')) { continue }
			CopyChecked $file.FullName (Join-Path $project $relative)
		}
		$scene = Clone $baseScene; $scene.name = 'Frame Replay ' + $definition.name
		$objects = [Collections.Generic.List[object]]::new()
		for($index=1; $index -le $definition.cameras; $index++) {
			$camera=Clone $cameras[0]; $camera.uuid='068c000000000000'+$index.ToString('x16'); $camera.name="Replay Camera $index"
			$objects.Add($camera)
		}
		for($index=1; $index -le $definition.cpu; $index++) { $objects.Add((AuthorActor $actors[0] $index $false)) }
		for($index=1; $index -le $definition.compact; $index++) { $objects.Add((AuthorActor $actors[0] (10+$index) $true)) }
		$staticGrids=@()
		if($definition.grid) {
			$gridBase = Join-Path $work 'bootstrap/grid/project'
			CopyChecked (Join-Path $gridBase 'renderer-efficiency-variants/grid-page-baseline.json') (Join-Path $project 'Assets/Models/RendererEfficiency_grid.gltf')
			CopyChecked (Join-Path $gridBase 'Assets/Models/RendererEfficiency_grid.gltf.meta') (Join-Path $project 'Assets/Models/RendererEfficiency_grid.gltf.meta')
			$gridScene = ReadJson (Join-Path $gridBase 'Assets/Scenes/Scene.tescene')
			$grid = @($gridScene.objects | Where-Object { $_.name -ceq 'SelectedGeometry' })
			Require ($grid.Count -eq 1) 'Maintained grid actor missing.'
			$staticGrids=@(AuthorStaticGrids $grid[0] $definition.gridInstances)
			foreach($instance in $staticGrids) { $objects.Add($instance) }
		}
		$expectedRenderables=$definition.cpu+$definition.compact+$definition.gridInstances
		$authoredFacts=GetAuthoredObjectFacts $objects.ToArray()
		Require ($authoredFacts.meshRenderComponentCount -eq $expectedRenderables) 'Authored native-renderable input count differs.'
		if($definition.name -ceq 'Mixed') { Require ($authoredFacts.meshRenderComponentCount -eq 19 -and $authoredFacts.meshRenderComponentCount -gt 16) 'Mixed must exceed the unchanged production inline16 threshold.' }
		WriteJson (Join-Path $evidence 'authored-inputs-before-import.json') $authoredFacts
		$scene.objects=$objects.ToArray(); WriteJson (Join-Path $project 'Assets/Scenes/Scene.tescene') $scene
		$closed=$null; $imports=[Collections.Generic.List[object]]::new()
		try {
			Launch $project $evidence
			$hero = ImportReal $project 'Models/ImportedHero.gltf'; $imports.Add($hero)
			Require ($hero.animationManifest.publishedAtomically -eq $true -and $hero.animationManifest.publishedGeneration -gt 0 -and $hero.animationManifest.candidateGeneration -eq $hero.animationManifest.publishedGeneration -and !$hero.animationManifest.cancelled -and !$hero.animationManifest.preservedLastKnownGood) 'Hero import lacks its current complete atomic animation publication.'
			$heroBindings=@{}
			foreach($selection in @(
				@{kind='DeformableMesh';name='ImportedHeroBody'},
				@{kind='Skeleton';name='ImportedHeroSkeleton'},
				@{kind='AnimationClip';name='ImportedHeroIdle'},
				@{kind='Material';name='ImportedHeroMaterial'}
			)) {
				$heroRows=@($hero.animationManifest.subAssets | Where-Object {$_.kind -ceq $selection.kind -and $_.name -ceq $selection.name})
				Require ($heroRows.Count -eq 1) "Actual hero child identity is missing or ambiguous: $($selection.kind)/$($selection.name)"
				Require ($heroRows[0].guid -cmatch '^[0-9a-f]{32}$' -and $heroRows[0].guid -cne ('0'*32)) 'Actual hero child GUID is invalid.'
				$heroBindings[$selection.kind]=$heroRows[0]
			}
			$fingerprint=$heroBindings.Skeleton.compatibilityFingerprint
			Require ($fingerprint -ne 0 -and $heroBindings.AnimationClip.compatibilityFingerprint -eq $fingerprint -and $heroBindings.DeformableMesh.compatibilityFingerprint -eq $fingerprint) 'Actual hero mesh, skeleton, and clip compatibility fingerprints disagree.'
			# Old aliases may still resolve a newly imported mesh, while old skeleton
			# and clip IDs name retained files with a different compatibility fingerprint.
			# Bind every actor/body to this one real import before saving the fixture.
			foreach($actor in $objects | Where-Object {$_.components.ContainsKey('AnimatorComponent')}) {
				SetProperty $actor.uuid 'AnimatorComponent' 'defaultClipAssetId' $heroBindings.AnimationClip.guid
				SetProperty $actor.uuid 'AnimatorComponent' 'skeletonOverrideAssetId' $heroBindings.Skeleton.guid
				SetProperty $actor.children[0].uuid 'MeshRenderComponent' 'meshId' $heroBindings.DeformableMesh.guid
				SetProperty $actor.children[0].uuid 'MeshRenderComponent' 'materialId' $heroBindings.Material.guid
			}
			WriteJson (Join-Path $evidence 'hero-bindings.json') $heroBindings
			if($definition.grid) {
				$receipt = ImportReal $project 'Models/RendererEfficiency_grid.gltf'; $imports.Add($receipt)
				$meshes=@($receipt.generatedMeshes | Where-Object {$_.name -ceq 'SelectedGeometry'})
				$materials=@($receipt.generatedMaterials | Where-Object {$_.name -ceq 'SelectedGeometryMaterial0'})
				Require ($meshes.Count -eq 1 -and $materials.Count -eq 1) 'Actual static grid child identity is ambiguous.'
				Require ($meshes[0].virtualGeometry.cooked -eq $true -and $meshes[0].virtualGeometry.sourceTriangles -eq 8192) 'Actual importer did not cook the maintained dense64-grid source.'
				foreach($instance in $staticGrids) {
					SetProperty $instance.uuid 'MeshRenderComponent' 'meshId' $meshes[0].guid
					SetProperty $instance.uuid 'MeshRenderComponent' 'materialId' $materials[0].guid
				}
			}
			$closed = CloseOwned $project
		} finally { FinishProcess $evidence }
		Require ($null -ne $closed) 'Project was not closed successfully.'
		$savedScene=ReadJson (Join-Path $project 'Assets/Scenes/Scene.tescene')
		$savedFacts=GetAuthoredObjectFacts $savedScene.objects
		Require ($savedFacts.meshRenderComponentCount -eq $expectedRenderables -and ($savedFacts.objectIds -join ',') -ceq ($authoredFacts.objectIds -join ',')) 'Saved scene object identities/renderable input counts changed.'
		WriteJson (Join-Path $evidence 'authored-inputs-after-close.json') $savedFacts
		# Derive registration only from actual importer records, retaining originals
		# first for compatibility with authored baseline IDs. Canonical rows win.
		$rows=[ordered]@{}
		foreach($row in (ReadJson (Join-Path $project 'Assets/Models/RendererEfficiency.animation-manifest.json')).subAssets) {
			if($row.kind -in @('DeformableMesh','Material','Skeleton','AnimationClip')) { $rows[$row.guid]=$row }
		}
		foreach($receipt in $imports) {
			foreach($row in $receipt.animationManifest.subAssets) { if($row.kind -in @('DeformableMesh','Material','Skeleton','AnimationClip')) {$rows[$row.guid]=$row} }
			foreach($pair in @(@{key='generatedMeshes';kind='Mesh'},@{key='generatedMaterials';kind='Material'})) {
				foreach($row in $receipt[$pair.key]) {
					# Deformable children retain their animation-manifest classification.
					if(!$rows.Contains($row.guid)) {$rows[$row.guid]=@{kind=$pair.kind;guid=$row.guid;compiledPath=$row.compiledPath}}
				}
			}
		}
		$registration=@()
		foreach($guid in @($rows.Keys | Sort-Object)) {
			$row=$rows[$guid]; Require ($guid -cmatch '^[0-9a-f]{32}$' -and $guid -cne ('0'*32)) 'Invalid actual asset GUID.'
			$relative=[string]$row.compiledPath
			Require (![IO.Path]::IsPathRooted($relative) -and !($relative.Replace('\','/').Split('/') -contains '..')) 'Cooked path escapes project.'
			$path=Full (Join-Path $project ('Build/Assets/'+$relative)); Require (Within $path $project) 'Cooked path escapes project.'
			Require ((Get-Item -LiteralPath $path).Length -gt 0) 'Cooked payload missing/empty.'; NoRedirect $path
			$registration+=@{kind=$row.kind;guid=$guid;compiledPath=$relative}
		}
		$manifestPath=Join-Path $profileRoot 'asset-manifest.json'
		WriteJson $manifestPath @{subAssets=$registration}
		$files=@(foreach($file in Get-ChildItem -LiteralPath $project -Recurse -File | Sort-Object FullName) {
			$pin=Pin $file.FullName; $pin.path=[IO.Path]::GetRelativePath($project,$file.FullName).Replace('\','/'); $pin
		})
		$closurePath=Join-Path $profileRoot 'project-closure.json'
		# Keep this receipt portable: the runner copies the pristine project per
		# process and reuses these identical controls with relative project paths.
		$manifestPin=Pin $manifestPath
		WriteJson $closurePath @{schemaVersion='frame-replay-project-closure-v1';profile=$definition.profile;variant=$definition.variant;files=$files;assetManifest=@{bytes=$manifestPin.bytes;sha256=$manifestPin.sha256};closedEditor=$closed;status='ClosedInputsPinned';qualification='Requested profile still requires real collector route and authority observations'}
		$summary.profiles+=@{name=$definition.name;profile=$definition.profile;variant=$definition.variant;projectRoot=$project;assetManifest=$manifestPath;projectClosure=$closurePath;closurePin=(Pin $closurePath)}
		WriteJson (Join-Path $work 'preparation.json') $summary
	}
	foreach($pin in $inputPins) { Require ((Hash $pin.path) -ceq $pin.sha256) 'Installed tool/script changed during preparation.' }
	$summary.status='ClosedInputsPrepared'; WriteJson (Join-Path $work 'preparation.json') $summary
	Write-Output "Prepared five closed replay input variants: $work/preparation.json; actual profile qualification remains pending."
} catch {
	$summary.error=$_.Exception.Message; WriteJson (Join-Path $work 'preparation.json') $summary
	throw
}
