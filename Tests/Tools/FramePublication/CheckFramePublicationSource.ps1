[CmdletBinding()]
param([string]$Root=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path)
$ErrorActionPreference='Stop'
$forbidden='048-refactor-frame-publication|(?<![A-Za-z0-9])US[0-9]+(?![A-Za-z0-9])|(?<![A-Za-z0-9])T[0-9]{3}(?![A-Za-z0-9])|legacy.*renderer|DX12|DirectX12|(?<![A-Za-z0-9])(?:NVIDIA|AMD)(?![A-Za-z0-9])|vendor-specific|spec kit|user story|staggered.*update|old.*animation.*publication'
# Audit only source-owned product and validation folders. External dependencies,
# generated build trees, local agent metadata, and uncommitted specifications are
# intentionally outside the engine naming contract and make recursive scans both
# noisy and unnecessarily slow.
$sourceFiles=@(
	'Engine/Renderer/RenderSystem.h',
	'Engine/Renderer/RenderSystem.cpp',
	'Engine/Renderer/Graph/RenderFramePublication.h',
	'Engine/Renderer/Graph/RenderFramePublication.cpp',
	'Engine/Renderer/RHI/RHIContext.h',
	'Engine/Renderer/RHI/Vulkan/VulkanCommandRecording.h',
	'Engine/Renderer/RHI/Vulkan/VulkanCommandRecording.cpp',
	'Engine/Animation/AnimationSystem.h',
	'Engine/Animation/AnimationSystem.cpp',
	'Engine/Animation/AnimationPresentationSystem.h',
	'Engine/Animation/AnimationPresentationSystem.cpp',
	'Engine/Animation/Runtime/AnimationAuthorityPublication.h',
	'Engine/Animation/Runtime/AnimationAuthorityPublication.cpp',
	'Engine/Animation/Runtime/AnimationFrameSidecars.h',
	'Engine/Profiling/Profiler.h',
	'Engine/Profiling/Profiler.cpp',
	'Engine/Profiling/ProfilerCapture.h',
	'Engine/Profiling/ProfilerCapture.cpp',
	'Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp'
)|ForEach-Object{Join-Path $Root $_}|Where-Object{Test-Path -LiteralPath $_}
$toolFiles=Get-ChildItem -LiteralPath (Join-Path $Root 'Tests/Tools/FramePublication') -File |
	Where-Object{$_.Name -ne 'CheckFramePublicationSource.ps1'}|Select-Object -ExpandProperty FullName
$sourceFiles=@($sourceFiles)+@($toolFiles)
$ripgrep=Get-Command rg -ErrorAction SilentlyContinue
if($null -ne $ripgrep){
	& $ripgrep.Source --pcre2 -n -i -e $forbidden @sourceFiles
	if($LASTEXITCODE -eq 0){exit 1}
	if($LASTEXITCODE -ne 1){throw "Source audit failed while scanning files (rg exit $LASTEXITCODE)."}
	Write-Output 'Frame publication source audit passed.'
	exit 0
}
$hits=$sourceFiles|Select-String -Pattern $forbidden -CaseSensitive:$false
if($hits){$hits|ForEach-Object{Write-Output $_.ToString()}; exit 1}
Write-Output 'Frame publication source audit passed.'
