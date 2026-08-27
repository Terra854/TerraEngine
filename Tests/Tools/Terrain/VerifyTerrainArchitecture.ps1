# VerifyTerrainArchitecture.ps1 - static terrain architecture guardrails.
#
# This check is intentionally conservative: it proves required seams are
# present, but never claims runtime scheduling or publication correctness.
[CmdletBinding()]
param([string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../')).Path)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()
function Read-Source([string]$relative) {
	$path = Join-Path $SourceRoot $relative
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $failures.Add("Missing source: $relative"); return '' }
	return Get-Content -Raw -LiteralPath $path
}
function Require-Pattern([string]$relative, [string]$pattern, [string]$message) {
	$content = Read-Source $relative
	if ($content -and $content -notmatch $pattern) { $failures.Add($message) }
}
function Reject-Pattern([string]$relative, [string]$pattern, [string]$message) {
	$content = Read-Source $relative
	if ($content -and $content -match $pattern) { $failures.Add($message) }
}
function Reject-TreePattern([string[]]$roots, [string]$pattern, [string]$message) {
	# Read complete files so this guard also catches multi-line raw shader strings.
	# Report every matching path to make a failure immediately actionable.
	$matches = [System.Collections.Generic.List[string]]::new()
	foreach ($root in $roots) {
		$absoluteRoot = Join-Path $SourceRoot $root
		if (-not (Test-Path -LiteralPath $absoluteRoot -PathType Container)) {
			$failures.Add("Missing source directory: $root")
			continue
		}
		foreach ($file in Get-ChildItem -LiteralPath $absoluteRoot -Recurse -File) {
			if ($file.Extension -notin @('.cpp', '.cc', '.cxx', '.h', '.hpp')) { continue }
			if ((Get-Content -Raw -LiteralPath $file.FullName) -match $pattern) {
				$matches.Add([IO.Path]::GetRelativePath($SourceRoot, $file.FullName).Replace('\', '/'))
			}
		}
	}
	if ($matches.Count -gt 0) { $failures.Add("$message Matches: $($matches -join ', ').") }
}

Require-Pattern 'Engine/Terrain/TerrainAssetStore.cpp' 'kTerrainRootMagic' 'Terrain persistence must use the shared canonical TERRAROOT1 magic.'
Require-Pattern 'Engine/Terrain/TerrainAssetStore.cpp' 'HashTerrainBytes\(bytes\)' 'Terrain root identity must hash the exact canonical document bytes.'
Require-Pattern 'Engine/File Formats/TerraEngineTerrainFile.h' 'kTerrainRootMagic\s*=\s*"TERRAROOT1"' 'The public terrain format contract must name TERRAROOT1 as the sole canonical root magic.'
Require-Pattern 'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp' 'DecodeTerrainRootManifest' 'Mounted terrain loading must use the shared bounded TERRAROOT1 decoder.'
foreach ($source in @(
	'Engine/Terrain/TerrainAssetStore.cpp',
	'Editor/TerraEngine-Editor/Terrain/TerrainCreationService.cpp',
	'Engine/Runtime/ContentMount/TerrainContentResolver.cpp',
	'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp')) {
	Reject-Pattern $source 'DecodeLegacy(?:Origin|Root)|TerrainOriginSource|roots[/\\]1\.json|starts_with\s*\(\s*"roots/"|(?:==|!=)\s*"TERRAINROOT2"' "$source must not retain a legacy or path-shaped compatibility decoder; reject old roots with recreate/recook guidance."
}
Reject-Pattern 'Engine/Terrain/TerrainAssetStore.cpp' 'nlohmann(?:::|/)json|#include\s*<nlohmann/json' 'The canonical TERRAROOT1 object codec must not reintroduce a JSON compatibility decoder.'

# Mounted loading must traverse every accepted shard/area and produce one
# immutable area packet per descriptor. A legacy origin mirror may remain for
# transitional callers, but it cannot be the authoritative runtime result.
Require-Pattern 'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.h' 'std::vector<MountedTerrainAreaRuntime>\s+areas' 'MountedTerrainRuntimeResult must expose the complete immutable area set.'
Require-Pattern 'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp' 'root->directoryShards' 'Mounted terrain loading must traverse every canonical directory shard.'
Require-Pattern 'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp' 'shard\.descriptors' 'Mounted terrain loading must traverse every area descriptor in each shard.'
Require-Pattern 'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp' 'completed\.areas\.push_back' 'Mounted terrain loading must retain every prepared area instead of only area {0,0}.'
Require-Pattern 'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp' 'CookTerrainPage' 'Each mounted area must prepare its page-backed terrain data.'
Reject-Pattern 'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp' 'DecodeImmutableOrigin|complete origin area' 'The mounted loader still contains an origin-only decoding path; publish the complete canonical area set.'

# Layer bounds are validation rules, not renderer truncation rules. The complete
# accepted enabled set must either publish or reject atomically.
Require-Pattern 'Engine/Terrain/TerrainLayerStack.cpp' 'enabledCount\s*==\s*0u\s*\|\|\s*enabledCount\s*>\s*Format::kTerrainMaximumEnabledLayers' 'Layer-stack validation must reject zero or more than sixteen enabled layers.'
Require-Pattern 'Engine/Terrain/TerrainAssetStore.cpp' 'enabledCount\s*==\s*0u\s*\|\|\s*enabledCount\s*>\s*Format::kTerrainMaximumEnabledLayers' 'TERRAROOT1 validation must reject an invalid enabled-layer count before publication.'
Require-Pattern 'Engine/Renderer/Terrain/TerrainMaterialRuntimeRegistry.h' 'layerCount\s*>\s*0u\s*&&\s*layerCount\s*<=\s*kTerrainGpuLayerLimit' 'Renderer material publication must validate the complete enabled-layer count.'
foreach ($source in @(
	'Engine/Terrain/TerrainMaterial.cpp',
	'Engine/Renderer/Terrain/TerrainMaterialRuntimeRegistry.cpp',
	'Engine/Renderer/Terrain/TerrainMaterialRuntimeRegistry.h',
	'Engine/Renderer/Terrain/TerrainRenderPackets.cpp')) {
	Reject-Pattern $source '(?:resize|erase)\s*\(\s*kTerrainGpuLayerLimit|std::min(?:<[^>]+>)?\s*\([^;\r\n]*(?:kTerrainGpuLayerLimit|kTerrainMaximumEnabledLayers)' "$source must reject excess enabled layers instead of silently truncating them."
}

# Terrain shaders remain dedicated files. Raw HLSL/GLSL in terrain C++ would
# evade shader compilation, installed-artifact checks, and source auditing.
foreach ($shader in @(
	'Engine/Shaders/Terrain/TerrainMaterial.hlsli',
	'Engine/Shaders/Terrain/TerrainVisibility.task.hlsl',
	'Engine/Shaders/Terrain/TerrainVisibility.mesh.hlsl',
	'Engine/Shaders/Terrain/TerrainVisibility.frag.hlsl',
	'Engine/Shaders/Terrain/TerrainSurface.comp.hlsl',
	'Editor/Shaders/Terrain/TerrainBrushPreview.vert.hlsl',
	'Editor/Shaders/Terrain/TerrainBrushPreview.frag.hlsl')) {
	Require-Pattern $shader '\S' "Required dedicated terrain shader source is empty: $shader"
}
Require-Pattern 'CMakeLists.txt' 'terraengine_add_terrain_shader' 'Terrain shader sources must compile through the dedicated CMake shader pipeline.'
Require-Pattern 'CMakeLists.txt' 'TERRAENGINE_TERRAIN_COMPILED_RUNTIME_SHADERS' 'Runtime terrain shader packages must be registered for installation.'
Require-Pattern 'CMakeLists.txt' 'TERRAENGINE_TERRAIN_COMPILED_EDITOR_SHADERS' 'Editor terrain shader packages must be registered for installation.'
Reject-TreePattern @(
	'Engine/Terrain',
	'Engine/Renderer/Terrain',
	'Engine/Renderer/RHI/Vulkan',
	'Editor/TerraEngine-Editor/Terrain') '(?s)(?:R"\w*\(|(?:const|constexpr)[^;\r\n]{0,160}(?:char|string)[^=]*=\s*")[^;]{0,8192}(?:#version|cbuffer\s+|Texture2D(?:<|\s)|RWTexture|SV_(?:Position|Target))' 'Terrain shader programs must not be embedded in C++.'

# Worker packet preparation must leave the descriptor incomplete. Only the
# Vulkan frame-fence completion path may flip uploadCompleted before protected
# virtual-geometry publication.
Require-Pattern 'Engine/Renderer/RHI/Vulkan/VulkanVirtualGeometryResources.cpp' 'descriptor\.uploadCompleted\s*=\s*false' 'Vulkan page candidates must begin with an incomplete upload descriptor.'
Require-Pattern 'Engine/Renderer/RHI/Vulkan/VulkanContext.cpp' 'vkWaitForFences' 'Vulkan page publication must retain an explicit GPU fence-completion proof.'
Require-Pattern 'Engine/Renderer/RHI/Vulkan/VulkanContext.cpp' 'descriptor\.uploadCompleted\s*=\s*true' 'The Vulkan fence-completion path must mark page descriptors upload-complete.'
foreach ($source in @(
	'Engine/Renderer/Terrain/TerrainRenderPackets.cpp',
	'Engine/Renderer/RHI/Vulkan/VulkanVirtualGeometryResources.cpp',
	'Engine/Renderer/RHI/Vulkan/VulkanUploadPlanning.cpp')) {
	Reject-Pattern $source 'descriptor\.uploadCompleted\s*=\s*true|candidate\.descriptor\s*=\s*\{[^}\r\n]*,\s*true\s*\}' "$source must not manufacture an upload-completed page descriptor before the Vulkan fence boundary."
}

# The bounded page ABI is source-registered so install rules can package the
# same version/checksum/range contract used by the cooker and decoder.
Require-Pattern 'Engine/Renderer/Terrain/TerrainPageFormat.h' 'kMagic' 'The terrain page ABI must expose a stable page magic.'
Require-Pattern 'Engine/Renderer/Terrain/TerrainPageFormat.h' 'kMaximumDecodedPageBytes' 'The terrain page ABI must bound decoded allocation size.'
Require-Pattern 'Engine/Renderer/Terrain/TerrainPageFormat.h' 'ValidateTerrainPageHeader' 'The terrain page ABI must expose bounded header validation.'
Require-Pattern 'cmake/TerraEngineTerrain.cmake' 'Engine/Renderer/Terrain/TerrainPageFormat\.h' 'TerrainPageFormat.h must be registered as an engine header so the install tree carries the page ABI.'

# Runtime terrain reads must remain mount-relative. These guards deliberately
# allow build-time CMake source paths while rejecting runtime source/build probes.
Require-Pattern 'Engine/Runtime/ContentMount/TerrainContentResolver.cpp' 'IsForbiddenTerrainPath' 'Terrain content resolution must reject unsafe runtime paths.'
Require-Pattern 'Engine/Runtime/ContentMount/TerrainContentResolver.cpp' 'path\.starts_with\("source/"\)' 'Terrain content resolution must explicitly reject source-tree logical paths.'
Require-Pattern 'Engine/Runtime/ContentMount/TerrainContentResolver.cpp' 'mount\.ReadAllChecked' 'Terrain roots and areas must resolve through ContentMount.'
foreach ($source in @(
	'Engine/Runtime/ContentMount/TerrainContentResolver.cpp',
	'Engine/Runtime/ContentMount/TerrainRuntimeProxyLoader.cpp',
	'Engine/Renderer/Terrain/TerrainPageCooker.cpp',
	'Engine/Renderer/Terrain/TerrainRenderPackets.cpp')) {
	Reject-Pattern $source 'CMAKE_SOURCE_DIR|PROJECT_SOURCE_DIR|std::filesystem::current_path\s*\(|(?:Engine|Editor)[/\\]Shaders[/\\]Terrain|Tests[/\\]Fixtures[/\\]Terrain' "$source must not probe source/build-tree terrain paths at runtime; use ContentMount or installed shader layout."
}

Require-Pattern 'Engine/Components/TerrainComponent.h' 'Component<TerrainComponent>' 'TerrainComponent must use Component<TerrainComponent>.'
Require-Pattern 'Engine/Components/TerrainComponent.h' 'kComponentName' 'TerrainComponent must expose kComponentName.'
Require-Pattern 'Engine/Components/TerrainComponent.h' 'static void reflect' 'TerrainComponent must provide reflection registration.'
foreach ($system in @(
	'Engine/Terrain/TerrainStreamingSystem',
	'Engine/Terrain/TerrainDerivedSystem',
	'Engine/Physics/Terrain/TerrainCollisionSystem',
	'Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem')) {
	Require-Pattern "$system.h" 'final\s*:\s*public\s+SystemsBase' "$system must inherit SystemsBase."
	Require-Pattern "$system.h" 'SchedulerDeclaration' "$system must declare scheduler resources."
	Require-Pattern "$system.cpp" 'DeclareFrameWorkloads' "$system must declare worker/protected workloads."
}
foreach ($source in @('Engine/Terrain/TerrainStreamingSystem.cpp','Engine/Terrain/TerrainDerivedSystem.cpp','Engine/Physics/Terrain/TerrainCollisionSystem.cpp','Editor/TerraEngine-Editor/Terrain/TerrainProceduralJobs.cpp')) {
	Require-Pattern $source 'cancel|Cancel|atomic' "$source must expose cancellation handling."
}
foreach ($root in @('Engine/Terrain','Engine/Physics/Terrain','Editor/TerraEngine-Editor/Terrain')) {
	$matches = Get-ChildItem -LiteralPath (Join-Path $SourceRoot $root) -Recurse -Include '*.cpp','*.h' -File | Select-String -Pattern 'ObjectManager|ComponentStorage'
	if ($matches) { $failures.Add("Worker-facing terrain code references live object/component storage under $root.") }
}
$result = [ordered]@{ schemaVersion = 1; status = if ($failures.Count -eq 0) { 'passed-static-guardrails' } else { 'failed' }; failures = @($failures); runtimeProof = 'not-provided-by-static-check' }
$result | ConvertTo-Json -Depth 8
if ($failures.Count -gt 0) { exit 1 }
