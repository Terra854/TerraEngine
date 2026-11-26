# Run from the repository root after installing the editor. The script creates a
# disposable project under WorkRoot and terminates only the editor it launches.
param(
	[string]$InstallRoot = 'out/build/RelWithDebInfo/Build',
	[string]$WorkRoot = 'out/validation/shader-collections'
)

$ErrorActionPreference = 'Stop'
$editor = (Resolve-Path (Join-Path $InstallRoot 'Editor')).Path
# The compatibility route uses an internal mesh shader.
if (!(Test-Path (Join-Path $editor 'Shaders/DefaultIndexed.mesh.tspf') -PathType Leaf)) {
	throw 'The built-in compatibility mesh shader package is missing.'
}
$work = [IO.Path]::GetFullPath((Join-Path $WorkRoot ('live-' + [guid]::NewGuid().ToString('N'))))
$project = Join-Path $work 'Shader Authoring'
$assets = Join-Path $project 'Assets'
$settings = Join-Path $project 'ProjectSettings'
$stages = Join-Path $assets 'Stages'
$cooked = Join-Path $project 'Build/Assets'
New-Item -Path $stages,$settings -ItemType Directory -Force | Out-Null
Copy-Item 'Tests/Fixtures/ExportRuntimeBaseline/ProjectSettings/ProjectSettings.terra' -Destination $settings
Set-Content -LiteralPath (Join-Path $project 'game.json') -Value '{"name":"Shader Authoring","version":"1.0.0","startupScene":""}'
# Separate loose sources exercise the three background compiles and installed include roots.
$names = [ordered]@{task='Default.task'; mesh='Default.mesh'; frag='Default.frag'}
foreach ($stage in $names.Keys) {
	Copy-Item "$editor/Project/Templates/ShaderSources/Default/$($names[$stage]).hlsl" "$stages/Authored.$stage.hlsl"
}
# A standalone post-process fragment shares the .frag.hlsl suffix. Its
# importer sidecar must keep it on the dedicated fullscreen ABI.
Copy-Item 'Engine/Shaders/PostProcess/Validation/PostProcessFixtureTint.frag.hlsl' "$assets/Standalone.frag.hlsl"
@{
	guid='f1000000000000000000000000000001'
	importerName='ShaderImporter'
	lastModified=0
	sourcePath='Standalone.frag.hlsl'
	compiledPath='Standalone.frag.tspf'
	settings=@{stage='ps';entryPoint='PSMain';target='ps_6_0';shaderUse='post-process'}
} | ConvertTo-Json -Depth 4 | Set-Content "$assets/Standalone.frag.hlsl.meta"
Set-Content "$assets/Empty.teshadercollection" '{"task":"","mesh":"","fragment":""}'
# An existing descriptor with a retired vertex override still loads its fragment.
Set-Content "$assets/FragmentOnly.teshadercollection" '{"task":"","mesh":"","fragment":"Stages/Authored.frag.hlsl","vertex":"Stages/Retired.vert.hlsl"}'
Set-Content "$assets/AllLinked.teshadercollection" '{"task":"Stages/Authored.task.hlsl","mesh":"Stages/Authored.mesh.hlsl","fragment":"Stages/Authored.frag.hlsl"}'
$prefs = Join-Path $work 'prefs.json'
@{ recentProjects=@(); autoBuildScripts=$false; editorMcpEnabled=$false; rendererDiagnosticsPreferencesInitialized=$true; vulkanValidationLayers=$false; renderDebugDiagnostics=$false } | ConvertTo-Json | Set-Content -LiteralPath $prefs
$stdout = Join-Path $work 'editor.stdout.log'
$stderr = Join-Path $work 'editor.stderr.log'
$p = Start-Process -FilePath (Join-Path $editor 'TerraEngine-Editor.exe') -WorkingDirectory $editor -ArgumentList ('--project "'+$project+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Environment @{
	TERRAENGINE_EDITOR_PREFS_SESSION_PATH = $prefs
	TERRAENGINE_SUPPRESS_PROJECT_RECENTS = '1'
}
function Wait-For([scriptblock]$Predicate, [string]$Name) {
	$clock=[Diagnostics.Stopwatch]::StartNew()
	while ($clock.Elapsed.TotalSeconds -lt 45) {
		if ($p.HasExited) { throw "Editor exited ($($p.ExitCode)) during $Name. Logs at $work" }
		if (& $Predicate) { return }
		Start-Sleep -Milliseconds 150
	}
	throw "Timed out waiting for $Name. Logs at $work"
}
function Count-Publications([string]$name) {
	@(Select-String -LiteralPath $stdout -Pattern "Finalized load for .*build[\\/]assets[\\/]${name}\.teshadercollection" -ErrorAction SilentlyContinue).Count
}
try {
	# A collection may load before its stage workers finish. Verify that every
	# collection eventually publishes and that an edit republishes its linked collection.
	Wait-For { @(Get-ChildItem -LiteralPath "$cooked/Stages" -Filter '*.tspf' -ErrorAction SilentlyContinue).Count -eq $names.Count } 'initial shader compilation'
	foreach ($stage in $names.Keys) {
		$file="$cooked/Stages/Authored.$stage.tspf"
		if([BitConverter]::ToUInt32([IO.File]::ReadAllBytes($file),0) -ne 0x46505354){throw "$file is not TSPF"}
	}
	'Each individual template compiled into a TSPF.'
	Wait-For { Test-Path -LiteralPath "$cooked/Standalone.frag.tspf" -PathType Leaf } 'standalone fragment compilation'
	$standalone = Get-Content "$assets/Standalone.frag.hlsl.meta" -Raw | ConvertFrom-Json
	if ($standalone.importerName -ne 'ShaderImporter' -or $standalone.settings.shaderUse -ne 'post-process') {
		throw 'The plain suffix replaced the explicit post-process importer.'
	}
	foreach ($collection in @('Empty','FragmentOnly','AllLinked')) {
		Wait-For { (Count-Publications $collection) -gt 0 } "$collection runtime publication"
	}
	'Empty, partial and fully linked collections published.'
	# Mutate only one side of each pipeline boundary and keep the other stages
	# linked. The resulting packages compile, but must never replace the valid
	# collection until the edited stage matches the default interface again.
	$payload = 'uint selectedVirtualLods[kTaskBatchSize];'
	$cases = @(
		@{stage='task'; from=$payload; to='uint selectedVirtualLods[kTaskBatchSize + 1u];'; detail='task-to-mesh payload'},
		@{stage='task'; from=$payload; to='int selectedVirtualLods[kTaskBatchSize];'; detail='task-to-mesh payload'},
		@{stage='mesh'; from=$payload; to='uint selectedVirtualLods[kTaskBatchSize + 1u];'; detail='task-to-mesh payload'},
		@{stage='mesh'; from=$payload; to='int selectedVirtualLods[kTaskBatchSize];'; detail='task-to-mesh payload'},
		@{stage='mesh'; from='kMaxMeshletVertices = 64'; to='kMaxMeshletVertices = 63'; detail='output capacity'},
		@{stage='mesh'; from='nointerpolation uint stablePrimitiveId : TEXCOORD6;'; to='nointerpolation int stablePrimitiveId : TEXCOORD6;'; detail='output count, location or type'},
		@{stage='frag'; from='nointerpolation uint stablePrimitiveId : TEXCOORD6'; to='nointerpolation int stablePrimitiveId : TEXCOORD6'; detail='input count, location or type'},
		@{stage='frag'; from='nointerpolation uint stablePrimitiveId : TEXCOORD6)'; to='nointerpolation uint stablePrimitiveId : TEXCOORD6, float2 extraInput : TEXCOORD20)'; detail='input count, location or type'; keepExtraInput=$true}
	)
	foreach ($case in $cases) {
		$path = "$stages/Authored.$($case.stage).hlsl"
		$original = [IO.File]::ReadAllText($path)
		$incompatible = $original.Replace($case.from,$case.to)
		if ($incompatible -eq $original) { throw "Update the interface test anchor for $path." }
		if ($case.keepExtraInput) {
			$anchor = 'TERRAENGINE_CONVENTIONAL_RETURN(shaded, stablePrimitiveId);'
			if (!$incompatible.Contains($anchor)) { throw 'Update the fragment output test anchor.' }
			$incompatible = $incompatible.Replace($anchor,'TERRAENGINE_CONVENTIONAL_RETURN(shaded + float4(extraInput, 0, 0), stablePrimitiveId);')
		}
		$pattern = 'Replacement shader interface mismatch.*' + [regex]::Escape($case.detail) + '.*' +
			[regex]::Escape("Authored.$($case.stage).tspf")
		$priorErrors = @(Select-String -LiteralPath $stdout -Pattern $pattern).Count
		$priorPublication = Count-Publications 'AllLinked'
		[IO.File]::WriteAllText($path,$incompatible)
		Wait-For { @(Select-String -LiteralPath $stdout -Pattern $pattern).Count -gt $priorErrors } "interface mismatch for $($case.stage)"
		if ((Count-Publications 'AllLinked') -ne $priorPublication) { throw 'An incompatible stage replaced the valid collection.' }
		[IO.File]::WriteAllText($path,$original)
		Wait-For { (Count-Publications 'AllLinked') -gt $priorPublication } "restored $($case.stage) publication"
	}
	'Task payload, mesh output and fragment input mismatches retained the last valid collection.'

	$frag="$stages/Authored.frag.hlsl"
	$compiled="$cooked/Stages/Authored.frag.tspf"
	$source=[IO.File]::ReadAllText($frag)
	$old=(Get-FileHash $compiled).Hash
	[IO.File]::WriteAllText($frag,$source+"`n#error TerraShaderCollectionSmokeInvalid`n")
	Wait-For { Select-String -LiteralPath $stdout,$stderr -SimpleMatch 'TerraShaderCollectionSmokeInvalid' -Quiet } 'invalid shader error'
	if((Get-FileHash $compiled).Hash -ne $old) {throw 'Failed compile replaced last good output'}
	$before=Count-Publications 'FragmentOnly'
	$emptyBefore=Count-Publications 'Empty'
	$changed=[datetime]::UtcNow
	# Change the result while keeping the stage input, output and bindings live.
	$edited = $source.Replace('TERRAENGINE_CONVENTIONAL_RETURN(shaded, stablePrimitiveId);',
		'TERRAENGINE_CONVENTIONAL_RETURN(shaded * float4(1.0, 0.9, 0.8, 1.0), stablePrimitiveId);')
	if ($edited -eq $source) { throw 'The default fragment changed. Update the test edit.' }
	[IO.File]::WriteAllText($frag,$edited)
	Wait-For { (Get-Item $compiled).LastWriteTimeUtc -gt $changed } 'automatic recompilation'
	Wait-For { (Count-Publications 'FragmentOnly') -gt $before } 'linked collection refresh'
	if ((Get-FileHash $compiled).Hash -eq $old) { throw 'The edited stage did not change the package' }
	if ((Count-Publications 'Empty') -ne $emptyBefore) { throw 'An unrelated collection refreshed' }
	'Invalid save preserved the last good output. Repaired save recompiled the stage and refreshed its linked collection.'
} finally {
	if(!$p.HasExited) { $null=$p.CloseMainWindow(); if(!$p.WaitForExit(6000)){Stop-Process -Id $p.Id} }
	"Smoke evidence: $work"
}
