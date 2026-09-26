# VerifyInstalledLocomotionContract.ps1 - fail-closed installed Feature 060 audit.
[CmdletBinding()]
param(
	[string]$InstallRoot = 'out/build/RelWithDebInfo/Build',
	[string]$OutputDirectory = 'out/evidence/locomotion/060/audit',
	[string]$SourceRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$install = [IO.Path]::GetFullPath($InstallRoot)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$installPrefix = $install.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if($output.Equals($install, [StringComparison]::OrdinalIgnoreCase) -or
	$output.StartsWith($installPrefix, [StringComparison]::OrdinalIgnoreCase)) {
	throw 'Installed audit evidence must be written outside the install tree.'
}
New-Item -ItemType Directory -Path $output -Force | Out-Null

$missing = [System.Collections.Generic.List[string]]::new()
$violations = [System.Collections.Generic.List[string]]::new()
$artifactHashes = [ordered]@{}

function Add-RequiredArtifact {
	param([string]$RelativePath)
	$path = Join-Path $install $RelativePath
	if(-not (Test-Path -LiteralPath $path -PathType Leaf)) {
		$missing.Add($RelativePath)
		return $null
	}
	$item = Get-Item -LiteralPath $path
	if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
		$violations.Add("Installed artifact is a reparse-point/source fallback: $RelativePath")
	}
	if($item.Length -eq 0) {
		$violations.Add("Installed artifact is empty: $RelativePath")
		return $null
	}
	$hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
	$artifactHashes[$RelativePath] = $hash
	return $path
}

function Test-BinaryLiteral {
	param([string]$Path, [string]$Literal)
	$bytes = [IO.File]::ReadAllBytes($Path)
	return [Text.Encoding]::ASCII.GetString($bytes).Contains($Literal, [StringComparison]::Ordinal)
}

$requiredArtifacts = @(
	'Editor/TerraEngine-Editor.exe',
	'Editor/TerraEngine-Export.exe',
	'Editor/ExportRuntime/TerraEngine-Launcher.exe',
	'Engine/TerraEngine.dll',
	'Editor/TerraEngine.dll',
	'Editor/ExportRuntime/TerraEngine.dll',
	'Editor/Managed/TerraEngine.CSharp.dll',
	'Tests/Fixtures/Locomotion/workloads.json',
	'Tests/Fixtures/Locomotion/Generated/fixture-assets.json',
	'Tests/Fixtures/Locomotion/Generated/expected-results.json',
	'Tests/Tools/Locomotion/InvokeLocomotionAuthoringAcceptance.ps1',
	'Tests/Tools/Locomotion/NewLocomotionWorkloadProject.ps1',
	'Tests/Tools/Locomotion/NewLocomotionFixtureAssets.ps1',
	'Tests/Tools/Locomotion/LocomotionFixtureMaterializer.cpp',
	'Tests/Tools/Locomotion/TerraEngine-LocomotionFixtureMaterializer.exe',
	'Tests/Tools/Locomotion/RunInstalledLocomotionAcceptance.ps1',
	'Tests/Tools/Locomotion/ManagedLocomotionAbiProbe.cs',
	'Tests/Tools/Locomotion/ManagedLocomotionAbiProbe.csproj',
	'Tests/Tools/Locomotion/ManagedLocomotionAuthoringSample.cs',
	'Tests/Tools/Locomotion/VerifyInstalledLocomotionContract.ps1'
)
$resolvedArtifacts = @{}
foreach($relative in $requiredArtifacts) {
	$resolvedArtifacts[$relative] = Add-RequiredArtifact -RelativePath $relative
}

# Component availability must reach both editor discovery and the exported native
# runtime, not just a header or source checkout. These strings are the registered
# component's durable type identity embedded in each shipping engine module.
$componentIdentityModules = @(
	'Engine/TerraEngine.dll', 'Editor/TerraEngine.dll', 'Editor/ExportRuntime/TerraEngine.dll'
)
foreach($relative in $componentIdentityModules) {
	$path = $resolvedArtifacts[$relative]
	if($null -eq $path) { continue }
	if(-not (Test-BinaryLiteral -Path $path -Literal 'CharacterLocomotionComponent')) {
		$violations.Add("Installed native module does not expose CharacterLocomotion component discovery identity: $relative")
	}
}

$animationShaderDirectory = Join-Path $install 'Editor/Shaders/Animation'
$animationShaders = @(
	'AnimationSample.comp.spv', 'AnimationBlend.comp.spv', 'AnimationHierarchy.comp.spv',
	'AnimationProcedural.comp.spv', 'AnimationMorph.comp.spv', 'AnimationBounds.comp.spv',
	'AnimationPalette.comp.spv', 'AnimationRagdoll.comp.spv', 'MotionMatchScore.comp.spv',
	'MotionMatchTopK.comp.spv'
)
$shaderAuditStatus = 'not-present'
$shaderHashes = [ordered]@{}
if(Test-Path -LiteralPath $animationShaderDirectory -PathType Container) {
	$shaderAuditStatus = 'verified'
	foreach($name in $animationShaders) {
		$relative = "Editor/Shaders/Animation/$name"
		$path = Add-RequiredArtifact -RelativePath $relative
		if($null -eq $path) { $shaderAuditStatus = 'failed'; continue }
		$shaderHashes[$relative] = $artifactHashes[$relative]
	}
}

# Check the installed managed ABI assembly's actual metadata strings. This is a
# static installed-byte audit; the separate CLR probe performs runtime layout tests.
$managedRelative = 'Editor/Managed/TerraEngine.CSharp.dll'
$managedAssembly = $resolvedArtifacts[$managedRelative]
$managedSymbols = @('CharacterLocomotion', 'NativeLocomotionCommand', 'NativeLocomotionObservation')
$managedSymbolResults = [ordered]@{}
if($null -ne $managedAssembly) {
	foreach($symbol in $managedSymbols) {
		$found = Test-BinaryLiteral -Path $managedAssembly -Literal $symbol
		$managedSymbolResults[$symbol] = $found
		if(-not $found) { $violations.Add("Installed managed locomotion symbol is missing: $symbol") }
	}
}

$workloadsRelative = 'Tests/Fixtures/Locomotion/workloads.json'
$workloadsPath = $resolvedArtifacts[$workloadsRelative]
$fixtureManifestRelative = 'Tests/Fixtures/Locomotion/Generated/fixture-assets.json'
$fixtureManifestPath = $resolvedArtifacts[$fixtureManifestRelative]
$workloadSha256 = $null
$fixtureAssetRecords = @()
$fixtureSha256 = $null
$fixtureManifestSha256 = $null
$fixtureAuditStatus = 'unavailable'
if($null -ne $workloadsPath -and $null -ne $fixtureManifestPath) {
	try {
		$workloadBytes = [IO.File]::ReadAllBytes($workloadsPath)
		$workloadSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($workloadBytes)).ToLowerInvariant()
		$workloads = [Text.Encoding]::UTF8.GetString($workloadBytes) | ConvertFrom-Json
		$fixtureBytes = [IO.File]::ReadAllBytes($fixtureManifestPath)
		$fixtureManifestSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($fixtureBytes)).ToLowerInvariant()
		$fixtureManifest = [Text.Encoding]::UTF8.GetString($fixtureBytes) | ConvertFrom-Json
		$fixtureSha256 = $fixtureManifestSha256
		if($workloads.schemaVersion -ne 1 -or $workloads.fixtureId -ne 'locomotion-060-v1' -or
			$workloads.generator.seed -ne 60060) {
			$violations.Add('Installed workload manifest does not identify locomotion-060-v1 / seed 60060.')
		}
		$generated = $workloads.generator.contentProvenance.t070GeneratedFixture
		if($generated.status -ne 'generated-production-codec-verified' -or
			$generated.expectedPath -ne $fixtureManifestRelative) {
			$violations.Add('Installed workload manifest does not pin the generated production-codec fixture manifest.')
		}
		if($fixtureManifest.schemaVersion -ne 1 -or $fixtureManifest.fixtureId -ne 'locomotion-060-assets-v1') {
			$violations.Add('Installed generated-asset manifest has an unsupported schema or fixture identity.')
		}
		$expectedResultsRelative = 'Tests/Fixtures/Locomotion/Generated/expected-results.json'
		if($fixtureManifest.expectedResultsPath -ne 'expected-results.json') {
			$violations.Add('Generated asset manifest does not pin the expected-results snapshot filename.')
		} else {
			$expectedResultsPath = $resolvedArtifacts[$expectedResultsRelative]
			if($null -ne $expectedResultsPath) {
				$expectedResultsHash = (Get-FileHash -LiteralPath $expectedResultsPath -Algorithm SHA256).Hash.ToLowerInvariant()
				if($fixtureManifest.expectedResultsSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
					$fixtureManifest.expectedResultsSha256.ToLowerInvariant() -ne $expectedResultsHash) {
					$violations.Add('Installed expected-results bytes do not match generated fixture provenance.')
				} else {
					$expectedResults = Get-Content -LiteralPath $expectedResultsPath -Raw | ConvertFrom-Json
					if($expectedResults.schemaVersion -ne 1 -or $expectedResults.fixtureId -ne 'locomotion-060-v1' -or
						$expectedResults.status -ne 'expected-only-not-executed') {
						$violations.Add('Generated expected-results snapshot must remain explicitly unexecuted and non-qualifying.')
					}
				}
			}
		}
		if($fixtureManifest.sourceManifestSha256 -ne $workloadSha256) {
			$violations.Add('Generated assets were not produced from the installed workload manifest bytes.')
		}

		$generatorRelative = 'Tests/Tools/Locomotion/LocomotionFixtureMaterializer.cpp'
		$generatorPath = $resolvedArtifacts[$generatorRelative]
		if($null -ne $generatorPath) {
			$expectedGeneratorHash = (Get-FileHash -LiteralPath $generatorPath -Algorithm SHA256).Hash.ToLowerInvariant()
			if($fixtureManifest.generatorSourceSha256 -ne $expectedGeneratorHash) {
				$violations.Add('Generated asset provenance does not match the installed fixture-generator bytes.')
			}
		}

		$requiredRoles = @('skeleton', 'idleClip', 'moveClip', 'rootClip', 'controller', 'ragdollProfile')
		$assets = @($fixtureManifest.assets)
		foreach($role in $requiredRoles) {
			$matches = @($assets | Where-Object { $_.role -eq $role })
			if($matches.Count -ne 1) {
				$violations.Add("Generated asset manifest must contain exactly one '$role' record.")
				continue
			}
			$asset = $matches[0]
			if($asset.assetId -notmatch '^[0-9a-fA-F]{32}$' -or $asset.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
				$violations.Add("Generated '$role' record must pin a 16-byte asset ID and a SHA-256 hash.")
				continue
			}
			$relativeAssetPath = [string]$asset.path
			if([IO.Path]::IsPathRooted($relativeAssetPath) -or $relativeAssetPath -match '(^|[\\/])\.\.([\\/]|$)') {
				$violations.Add("Generated '$role' path must remain relative beneath the installed fixture root.")
				continue
			}
			$installRelativeAsset = "Tests/Fixtures/Locomotion/Generated/$($relativeAssetPath.Replace('\', '/'))"
			$assetPath = Add-RequiredArtifact -RelativePath $installRelativeAsset
			$actualAssetHash = $null
			if($null -ne $assetPath) {
				$actualAssetHash = (Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()
				if($actualAssetHash -ne $asset.sha256.ToLowerInvariant()) {
					$violations.Add("Installed cooked '$role' bytes do not match their fixture manifest SHA-256.")
				}
			}
			if($generated.assetIds.$role -ne $asset.assetId -or $generated.sha256.$role -ne $asset.sha256) {
				$violations.Add("Workload provenance and generated '$role' identity/hash disagree.")
			}
			$fixtureAssetRecords += [ordered]@{
				role = $role; assetId = $asset.assetId.ToLowerInvariant(); path = $installRelativeAsset
				sha256 = $asset.sha256.ToLowerInvariant(); status = if($actualAssetHash -eq $asset.sha256.ToLowerInvariant()) { 'verified' } else { 'unavailable' }
			}
		}
		$generatedAssetDirectory = Join-Path $install 'Tests/Fixtures/Locomotion/Generated/Assets/Animation'
		if(Test-Path -LiteralPath $generatedAssetDirectory -PathType Container) {
			$sourceExtensions = @('.fbx', '.gltf', '.glb', '.dae', '.blend', '.meta')
			foreach($file in Get-ChildItem -LiteralPath $generatedAssetDirectory -Recurse -File) {
				if([IO.Path]::GetExtension($file.Name).ToLowerInvariant() -in $sourceExtensions) {
					$violations.Add("Source/editor asset was installed beside generated runtime fixtures: $($file.FullName)")
				}
			}
		}
		$fixtureAuditStatus = if($violations.Count -eq 0) { 'verified' } else { 'failed' }
	} catch {
		$violations.Add("Installed locomotion fixture metadata is invalid: $($_.Exception.Message)")
	}
}

# Existing committed source fixtures are also pinned by their declared SHA-256;
# compare the installed copies without confusing them with generated T070 outputs.
$sourceFixtureResults = @()
if($null -ne $workloadsPath) {
	try {
		$workloads = Get-Content -LiteralPath $workloadsPath -Raw | ConvertFrom-Json
		foreach($source in @($workloads.generator.contentProvenance.animationFixtures)) {
			$sourcePairs = @(
				@{ Path = 'path'; Hash = 'sha256' },
				@{ Path = 'skeletonPath'; Hash = 'skeletonSha256' },
				@{ Path = 'sourcePath'; Hash = 'sourceSha256' }
			)
			foreach($pair in $sourcePairs) {
				$pathField = $pair.Path
				$hashField = $pair.Hash
				$pathProperty = $source.PSObject.Properties[$pathField]
				$hashProperty = $source.PSObject.Properties[$hashField]
				if($null -eq $pathProperty -or $null -eq $hashProperty) { continue }
				if([string]::IsNullOrWhiteSpace([string]$pathProperty.Value) -or
					[string]::IsNullOrWhiteSpace([string]$hashProperty.Value)) { continue }
				$relative = [string]$pathProperty.Value
				if([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)') {
					$violations.Add("Pinned source fixture path escapes the install root: $relative")
					continue
				}
				$path = Add-RequiredArtifact -RelativePath $relative
				if($null -eq $path) { continue }
				$actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
				$expected = ([string]$hashProperty.Value).ToLowerInvariant()
				if($actual -ne $expected) { $violations.Add("Installed source fixture hash mismatch: $relative") }
				$sourceFixtureResults += [ordered]@{ id = $source.id; path = $relative; sha256 = $actual; status = if($actual -eq $expected) { 'verified' } else { 'failed' } }
			}
		}
	} catch {
		$violations.Add("Pinned source fixture audit failed: $($_.Exception.Message)")
	}
}

# Installed tools/manifests are data, not instructions: reject absolute checkout
# paths and build-time CMake source selectors that could create runtime fallback.
if([string]::IsNullOrWhiteSpace($SourceRoot)) {
	$sourceCandidate = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../'))
	if(Test-Path -LiteralPath (Join-Path $sourceCandidate 'CMakeLists.txt') -PathType Leaf) {
		$SourceRoot = $sourceCandidate
	}
}
$sourceFallbackArtifacts = [System.Collections.Generic.List[string]]::new()
$sourcePathPattern = '(?im)(?:[A-Za-z]:[\\/]|/(?:home|Users|workspace|workspaces|mnt)/)[^''"\r\n]*(?:Engine|Editor|Tests)[\\/]'
foreach($relative in $requiredArtifacts) {
	if([IO.Path]::GetExtension($relative).ToLowerInvariant() -notin @('.ps1', '.json', '.md', '.cs', '.csproj')) { continue }
	if($relative -eq 'Tests/Tools/Locomotion/VerifyInstalledLocomotionContract.ps1') { continue }
	$path = $resolvedArtifacts[$relative]
	if($null -eq $path) { continue }
	$content = Get-Content -LiteralPath $path -Raw
	$hasSourceRoot = -not [string]::IsNullOrWhiteSpace($SourceRoot) -and
		$content.IndexOf([IO.Path]::GetFullPath($SourceRoot), [StringComparison]::OrdinalIgnoreCase) -ge 0
	if($hasSourceRoot -or $content -match 'CMAKE_SOURCE_DIR|PROJECT_SOURCE_DIR|\$\{CMAKE_CURRENT_SOURCE_DIR\}' -or
		$content -match $sourcePathPattern) {
		$sourceFallbackArtifacts.Add($relative)
		$violations.Add("Installed text artifact retains an absolute source/build-tree fallback: $relative")
	}
}

$hasFailures = $missing.Count -gt 0 -or $violations.Count -gt 0
$managedStatus = if($missing -contains $managedRelative) { 'unavailable' }
	elseif(@($managedSymbolResults.Values | Where-Object { -not $_ }).Count -gt 0) { 'failed' }
	else { 'verified' }
$componentStatus = if(@($componentIdentityModules | Where-Object { $missing -contains $_ }).Count -gt 0) { 'unavailable' }
	elseif(@($violations | Where-Object { $_ -match 'component discovery identity' }).Count -gt 0) { 'failed' }
	else { 'verified' }
$result = [ordered]@{
	schemaVersion = [ordered]@{ major = 1; minor = 0 }
	status = if($hasFailures) { 'unavailable' } else { 'available' }
	installRoot = $install
	requiredArtifacts = $requiredArtifacts
	missingArtifacts = @($missing)
	contractViolations = @($violations)
	componentDiscoveryAudit = [ordered]@{ modules = $componentIdentityModules; componentName = 'CharacterLocomotionComponent'; status = $componentStatus }
	managedSymbolAudit = [ordered]@{ assembly = $managedRelative; symbols = $managedSymbolResults; status = $managedStatus }
	shaderAudit = [ordered]@{ directoryPresent = (Test-Path -LiteralPath $animationShaderDirectory -PathType Container); expected = $animationShaders; hashes = $shaderHashes; status = $shaderAuditStatus }
	fixtureAudit = [ordered]@{
		workloadId = 'locomotion-060-v1'; seed = 60060; workloadManifestSha256 = $workloadSha256
		generatedManifest = $fixtureManifestRelative; generatedManifestSha256 = $fixtureManifestSha256
		generatedAssets = @($fixtureAssetRecords); sourceFixtures = @($sourceFixtureResults); status = $fixtureAuditStatus
	}
	sourceFallbackAudit = [ordered]@{ artifacts = @($sourceFallbackArtifacts); status = if($sourceFallbackArtifacts.Count -eq 0) { 'clear' } else { 'failed' } }
	artifactHashes = $artifactHashes
	note = 'This audit verifies installed artifact identity and fixture provenance only; it does not claim editor/export execution or performance qualification.'
}
$resultPath = Join-Path $output 'locomotion-installed-contract.json'
$result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $resultPath -Encoding utf8
$result | ConvertTo-Json -Depth 16
if($hasFailures) { exit 1 }
