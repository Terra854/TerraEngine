[CmdletBinding()]
param([Alias('RepositoryRoot')][string]$Root = (Resolve-Path "$PSScriptRoot\..\..\..").Path)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Audit only feature-owned implementation surfaces. TerraEngine contains existing
# renderer, texture, networking, and water systems whose vocabulary and worker
# choices are unrelated to navigation and therefore cannot establish scope creep.
$include = @(
	'Engine/Navigation',
	'Engine/Managed/Source/Navigation',
	'Editor/TerraEngine-Editor/Navigation',
	'Editor/TerraEngine-Editor/Windows/NavigationWindow.cpp',
	'Editor/TerraEngine-Editor/Windows/NavigationWindow.h',
	'Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp',
	'Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.h',
	'cmake/TerraEngineRecast.cmake'
)
$exclude = @('VerifyNavigationArchitecture.ps1','External/recastnavigation')
$scopeRules = @(
	'\bflying\b','movement.mode','mixed.mode','\bsea\b','\bwater\b','\bswimming\b',
	'behavior.tree','\bcombat\b','\bnetworking\b','predictive.platform','new.locomotion',
	'DetourCrowd','DetourTileCache','DebugUtils'
)
$workerRules = @('std::thread','new\s+ThreadPool')
$providerRules = @('\b(dtNavMesh|dtNavMeshQuery|dtPolyRef|rcConfig)\b')
$managedPackageRules = @('<PackageReference[^>]+(?:Navigation|Recast|Detour)')
$files = foreach($entry in $include) {
	$path = Join-Path $Root $entry
	if(Test-Path -LiteralPath $path -PathType Leaf) { Get-Item -LiteralPath $path; continue }
	if(Test-Path -LiteralPath $path -PathType Container) {
		Get-ChildItem -LiteralPath $path -Recurse -File -ErrorAction SilentlyContinue |
			Where-Object {
				($_.FullName -notmatch '[\\/](bin|obj|out|\.git)[\\/]') -and
				($_.Name -eq 'CMakeLists.txt' -or $_.Extension -in @('.h','.hpp','.cpp','.cs','.csproj','.cmake'))
			}
	}
}
$violations = foreach($file in $files) {
	if($exclude | Where-Object { $file.FullName -like "*$_*" }) { continue }
	Select-String -LiteralPath $file.FullName -Pattern $scopeRules -CaseSensitive:$false |
		Where-Object { $_.Line -notmatch '^\s*(#|//|/\*|\*)' } |
		ForEach-Object { "$($file.FullName):$($_.LineNumber): $($_.Line.Trim())" }
	Select-String -LiteralPath $file.FullName -Pattern $workerRules -CaseSensitive:$false |
		Where-Object { $_.Line -notmatch '^\s*(#|//|/\*|\*)' } |
		ForEach-Object { "$($file.FullName):$($_.LineNumber): $($_.Line.Trim())" }
	# Provider types are legal only inside the private backend translation units.
	# Public headers and the rest of the engine must remain provider-neutral.
	if($file.FullName -notmatch '[\\/]Engine[\\/]Navigation[\\/]Backend[\\/].+\.cpp$') {
		Select-String -LiteralPath $file.FullName -Pattern $providerRules -CaseSensitive:$true |
			Where-Object { $_.Line -notmatch '^\s*(#|//|/\*|\*)' } |
			ForEach-Object { "$($file.FullName):$($_.LineNumber): $($_.Line.Trim())" }
	}
}
# A developer-supplied managed navigation package would appear as a package
# reference, not merely as TerraEngine's built-in Navigation source directory.
$managedProject = Join-Path $Root 'Engine/Managed/TerraEngine.csproj'
if(Test-Path -LiteralPath $managedProject) {
	$violations += Select-String -LiteralPath $managedProject -Pattern $managedPackageRules -CaseSensitive:$false |
		ForEach-Object { "${managedProject}:$($_.LineNumber): $($_.Line.Trim())" }
}
if($violations) { $violations | Sort-Object -Unique | Write-Error; exit 1 }
Write-Host 'Navigation architecture audit passed.'
