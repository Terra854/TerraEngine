[CmdletBinding()]
param([Alias('RepositoryRoot')][string]$Root = (Resolve-Path "$PSScriptRoot\..\..\..").Path)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Source audits are deliberately narrow. TerraEngine contains existing renderer,
# texture, networking, and water systems whose vocabulary and worker choices are
# unrelated to navigation and therefore cannot establish scope creep.
$sourceInclude = @(
	'Engine/Navigation',
	'Engine/Managed/Source/Navigation',
	'Editor/TerraEngine-Editor/Navigation',
	'Editor/TerraEngine-Editor/Windows/NavigationWindow.cpp',
	'Editor/TerraEngine-Editor/Windows/NavigationWindow.h',
	'Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp',
	'Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.h',
	'cmake/TerraEngineRecast.cmake'
)
$sourceExclude = @('VerifyNavigationArchitecture.ps1','External/recastnavigation')
$retiredFiles = @(
	'Engine/Components/Navigation/NavigationAgentComponent.h',
	'Engine/Components/Navigation/NavigationSourceComponent.h',
	'Engine/Components/Navigation/NavigationConnectionComponent.h',
	'Engine/Managed/Source/Navigation/NavigationAgentComponent.cs',
	'Engine/Navigation/Ground/GroundTraversalConnectionStore.h',
	'Engine/Navigation/Ground/GroundTraversalConnectionStore.cpp'
)
$retiredRules = @(
	'NavigationAgentComponent','NavigationSourceComponent','NavigationConnectionComponent',
	'NavigationAgentHandle','NativeNavigationAgent','QueueSetDestination','SynchronizeSceneObjectAgents',
	'\.tenavsource',
	'Navigation(?:Agent|Source|Connection)(?:Component)?(?:Compatibility|Migration|Alias)'
)
$scopeRules = @(
	'\bflying\b','movement.mode','mixed.mode','\bsea\b','\bwater\b','\bswimming\b',
	'behavior.tree','\bcombat\b','\bnetworking\b','predictive.platform','new.locomotion',
	'DetourCrowd','DetourTileCache','DebugUtils'
)
$workerRules = @('std::thread','new\s+ThreadPool')
$providerRules = @('\b(dtNavMesh|dtNavMeshQuery|dtPolyRef|rcConfig)\b')
$managedPackageRules = @('<PackageReference[^>]+(?:Navigation|Recast|Detour)')

# An installed tree has binaries, managed assemblies, documentation, and support
# scripts rather than the source-layout directories above. Keep the two surface
# definitions explicit so a misplaced -Root cannot silently produce an empty audit.
$sourceMarkers = @('Engine/Navigation','Engine/Managed/Source/Navigation')
$installedMarkers = @('Engine','Editor','Binaries/Managed','Docs','Tests/Tools/Navigation')
$hasSourceSurface = @($sourceMarkers | Where-Object { Test-Path -LiteralPath (Join-Path $Root $_) }).Count -gt 0
$hasInstalledSurface = @($installedMarkers | Where-Object { Test-Path -LiteralPath (Join-Path $Root $_) }).Count -gt 0
$violations = [System.Collections.Generic.List[string]]::new()

function Add-Matches {
	param(
		[System.Collections.Generic.List[string]]$Destination,
		[System.IO.FileInfo]$File,
		[string[]]$Patterns,
		[string]$Description,
		[bool]$CaseSensitive = $false
	)

	foreach($match in @(Select-String -LiteralPath $File.FullName -Pattern $Patterns -CaseSensitive:$CaseSensitive)) {
		if($match.Line -match '^\s*(#|//|/\*|\*)') { continue }
		$Destination.Add("$($File.FullName):$($match.LineNumber): ${Description}: $($match.Line.Trim())")
	}
}

function Test-InstalledBinaryLiteral {
	param([string]$Path, [string]$Literal)

	# Native exports and managed metadata may encode a string as UTF-8 or UTF-16.
	# The runtime string search avoids requiring a developer-machine symbol utility
	# and stays practical for installed PDB files.
	$bytes = [System.IO.File]::ReadAllBytes($Path)
	foreach($encoding in @([System.Text.Encoding]::UTF8, [System.Text.Encoding]::Unicode)) {
		if($encoding.GetString($bytes).IndexOf($Literal, [System.StringComparison]::Ordinal) -ge 0) {
			return $true
		}
	}
	return $false
}

if(-not $hasSourceSurface -and -not $hasInstalledSurface) {
	$violations.Add("No recognized source or installed navigation audit surface exists beneath: $Root")
}

if($hasSourceSurface) {
	$sourceFiles = foreach($entry in $sourceInclude) {
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
	foreach($file in $sourceFiles) {
		if($sourceExclude | Where-Object { $file.FullName -like "*$_*" }) { continue }
		Add-Matches -Destination $violations -File $file -Patterns $scopeRules -Description 'out-of-scope navigation behavior'
		Add-Matches -Destination $violations -File $file -Patterns $workerRules -Description 'undeclared worker creation'
		# Provider types are legal only inside the private backend translation units.
		# Public headers and the rest of the engine must remain provider-neutral.
		if($file.FullName -notmatch '[\\/]Engine[\\/]Navigation[\\/]Backend[\\/].+\.cpp$') {
			Add-Matches -Destination $violations -File $file -Patterns $providerRules -Description 'provider type outside backend' -CaseSensitive $true
		}
	}

	# A developer-supplied managed navigation package would appear as a package
	# reference, not merely as TerraEngine's built-in Navigation source directory.
	$managedProject = Join-Path $Root 'Engine/Managed/TerraEngine.csproj'
	if(Test-Path -LiteralPath $managedProject) {
		Add-Matches -Destination $violations -File (Get-Item -LiteralPath $managedProject) -Patterns $managedPackageRules -Description 'managed navigation package reference'
	}
	foreach($retiredFile in $retiredFiles) {
		$absolute = Join-Path $Root $retiredFile
		if(Test-Path -LiteralPath $absolute) { $violations.Add("Retired production file still exists: $absolute") }
	}
	# Retired vocabulary is allowed in negative tests and documentation, but not in
	# production or package implementation surfaces.
	foreach($entry in @('Engine/Navigation','Engine/Managed/Source/Navigation','Engine/Core/Object/ComponentRegistry.cpp','Engine/Scene/Serializer.cpp','Engine/Runtime/ExportStartupCoordinator.cpp','Editor/TerraEngine-Editor/Navigation','Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp','CMakeLists.txt')) {
		$path = Join-Path $Root $entry
		if(-not (Test-Path -LiteralPath $path)) { continue }
		Get-ChildItem -LiteralPath $path -Recurse -File -ErrorAction SilentlyContinue |
			Where-Object { $_.FullName -notmatch '[\\/](bin|obj|out|\.git|Tests|Docs)[\\/]' -and $_.Extension -in @('.h','.hpp','.cpp','.cs','.csproj','.cmake','.json') } |
			ForEach-Object {
				foreach($match in @(Select-String -LiteralPath $_.FullName -Pattern $retiredRules -CaseSensitive:$false)) {
					$violations.Add("$($_.FullName):$($match.LineNumber): retired navigation compatibility/authoring surface: $($match.Line.Trim())")
				}
			}
	}
}

if($hasInstalledSurface -and -not $hasSourceSurface) {
	# Installed headers, support scripts, manifests, and docs are visible product
	# surfaces. Exclude this verifier and negative fixtures because they describe
	# forbidden identifiers deliberately rather than publishing an old API.
	$installedTextRoots = @('include','Docs','Tests/Tools/Navigation','Engine','Editor','Binaries/Managed','Launcher')
	$installedTextFiles = foreach($entry in $installedTextRoots) {
		$path = Join-Path $Root $entry
		if(Test-Path -LiteralPath $path -PathType Container) {
			Get-ChildItem -LiteralPath $path -Recurse -File -ErrorAction SilentlyContinue |
				Where-Object {
					$_.Name -ne 'VerifyNavigationArchitecture.ps1' -and
					$_.FullName -notmatch '[\\/](Fixtures|Negative|bin|obj|out)[\\/]' -and
					$_.Extension -in @('.h','.hpp','.hxx','.ps1','.json','.md','.txt','.xml','.manifest')
				}
		}
	}
	foreach($file in $installedTextFiles) {
		foreach($match in @(Select-String -LiteralPath $file.FullName -Pattern $retiredRules -CaseSensitive:$false)) {
			# Documentation and test fixtures can only describe a retired spelling as
			# a rejection/removal rule; any other installed occurrence is a product API.
			$isNegativeExplanation = $file.Extension -in @('.md','.txt') -and $match.Line -match '(?i)\b(reject(?:ed|ion)?|remove[ds]?|retir(?:e|ed)|unsupported|must not|no longer)\b'
			if(-not $isNegativeExplanation) {
				$violations.Add("$($file.FullName):$($match.LineNumber): retired navigation spelling in installed product surface: $($match.Line.Trim())")
			}
		}
	}

	$installedFiles = @($installedTextFiles) + @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
		Where-Object { $_.FullName -match '[\\/](Engine|Editor|Binaries|Launcher|include|Docs|Tests[\\/]Tools[\\/]Navigation)[\\/]' })
	foreach($retiredName in @($retiredFiles | ForEach-Object { Split-Path $_ -Leaf } | Sort-Object -Unique)) {
		foreach($file in @($installedFiles | Where-Object { $_.Name -eq $retiredName })) {
			$violations.Add("Retired installed navigation file still exists: $($file.FullName)")
		}
	}

	$nativeArtifacts = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
		Where-Object {
			$_.FullName -match '[\\/](Engine|Editor|Launcher|Tests[\\/]Tools[\\/]Navigation)[\\/]' -and
			# PDBs preserve source-level negative-test strings by design. Audit the
			# shipped native modules instead, whose exported ABI strings are product API.
			# The acceptance executable is also a negative-input fixture, while its
			# colocated TerraEngine.dll remains a required installed product scan.
			$_.Extension -in @('.dll','.exe') -and $_.Name -like 'TerraEngine*' -and
			$_.Name -ne 'TerraEngineNavigationAcceptance.exe'
		})
	$managedArtifacts = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
		Where-Object {
			$_.FullName -match '[\\/](Binaries[\\/]Managed|Editor[\\/]Managed)[\\/]' -and
			$_.Name -eq 'TerraEngine.CSharp.dll'
		})
	if($nativeArtifacts.Count -eq 0) { $violations.Add("Installed navigation audit found no native TerraEngine artifact beneath: $Root") }
	if($managedArtifacts.Count -eq 0) { $violations.Add("Installed navigation audit found no managed TerraEngine assembly beneath: $Root") }
	$binaryLiterals = @('NavigationAgentComponent','NavigationSourceComponent','NavigationConnectionComponent','NavigationAgentHandle','NativeNavigationAgent','QueueSetDestination','SynchronizeSceneObjectAgents','.tenavsource')
	foreach($artifact in @($nativeArtifacts) + @($managedArtifacts)) {
		foreach($literal in $binaryLiterals) {
			if(Test-InstalledBinaryLiteral -Path $artifact.FullName -Literal $literal) {
				$violations.Add("$($artifact.FullName): retired navigation ABI/symbol string '$literal'")
			}
		}
	}
}

if($violations.Count -gt 0) { $violations | Sort-Object -Unique | Write-Error; exit 1 }
Write-Host 'Navigation architecture audit passed for the available source/install surfaces.'
