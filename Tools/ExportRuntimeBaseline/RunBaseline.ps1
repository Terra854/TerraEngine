[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()]
	[string]$BuildDir = "out/build/Debug",

	[string]$InstallRoot = "",

	[ValidateNotNullOrEmpty()]
	[string]$WorkRoot = "out/ci/export-runtime-baseline",

	[ValidateNotNullOrEmpty()]
	[string]$FixtureRoot = "Tests/Fixtures/ExportRuntimeBaseline",

	[ValidateNotNullOrEmpty()]
	[string]$PhysicsFixtureRoot = "Tests/Fixtures/PhysicsExportRuntime",

	[ValidatePattern("^[0-9A-Fa-f]{32}$")]
	[string]$PackageGuid = "11111111222222223333333344444444",

	[ValidatePattern("^[0-9A-Fa-f]{32}$")]
	[string]$ShaderGuid = "00000000000000000000000000000001",

	[ValidatePattern("^[A-Za-z0-9][A-Za-z0-9._-]*$")]
	[string]$RunId = "export-runtime-baseline"
)

# This script is the repository's installed-tree export gate. Every phase uses
# the output from the phase before it, so the orchestration is intentionally
# ordered; the production compilers and TEPAK packager perform their heavy work
# through their own bounded worker pools.
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
	# Native failures are checked after their complete stdout/stderr stream has
	# been captured, which preserves the stage log that diagnoses the failure.
	$PSNativeCommandUseErrorActionPreference = $false
}

function Get-AbsolutePath {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path,

		[Parameter(Mandatory = $true)]
		[string]$BasePath
	)

	if ([System.IO.Path]::IsPathRooted($Path)) {
		return [System.IO.Path]::GetFullPath($Path)
	}
	return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Assert-PathIsBelow {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Parent,

		[Parameter(Mandatory = $true)]
		[string]$Child,

		[Parameter(Mandatory = $true)]
		[string]$Description
	)

	$parentPrefix = [System.IO.Path]::GetFullPath($Parent).TrimEnd(
		[System.IO.Path]::DirectorySeparatorChar,
		[System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
	$childFullPath = [System.IO.Path]::GetFullPath($Child)
	if (-not $childFullPath.StartsWith($parentPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "$Description must stay below '$parentPrefix'; received '$childFullPath'."
	}
}

function Resolve-InstalledExecutable {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Directory,

		[Parameter(Mandatory = $true)]
		[string]$BaseName
	)

	# Prefer the host-native Windows name, but keep the helper usable for local
	# source-level smoke tests on the portable CLI implementation.
	$candidates = @(
		(Join-Path $Directory "$BaseName.exe"),
		(Join-Path $Directory $BaseName)
	)
	foreach ($candidate in $candidates) {
		if (Test-Path -LiteralPath $candidate -PathType Leaf) {
			return (Resolve-Path -LiteralPath $candidate).Path
		}
	}
	throw "Required installed executable '$BaseName' was not found under '$Directory'. Build the install target first."
}

function Invoke-LoggedNative {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Stage,

		[Parameter(Mandatory = $true)]
		[string]$FilePath,

		[Parameter(Mandatory = $true)]
		[string[]]$Arguments,

		[Parameter(Mandatory = $true)]
		[string]$LogPath
	)

	"[$Stage] $FilePath $($Arguments -join ' ')" | Set-Content -LiteralPath $LogPath -Encoding utf8
	$previousErrorActionPreference = $ErrorActionPreference
	try {
		# Windows PowerShell surfaces redirected native stderr as non-terminating
		# ErrorRecord objects. Continue here so human diagnostics are logged while
		# the native exit code remains the authoritative failure signal.
		$ErrorActionPreference = "Continue"
		& $FilePath @Arguments 2>&1 |
			ForEach-Object {
				$line = $_.ToString()
				$line | Tee-Object -FilePath $LogPath -Append
			}
		$nativeExitCode = $LASTEXITCODE
	}
	finally {
		$ErrorActionPreference = $previousErrorActionPreference
	}
	if ($nativeExitCode -ne 0) {
		throw "Stage '$Stage' failed with exit code $nativeExitCode. See '$LogPath'."
	}
}

function Invoke-LoggedJsonNative {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Stage,

		[Parameter(Mandatory = $true)]
		[string]$FilePath,

		[Parameter(Mandatory = $true)]
		[string[]]$Arguments,

		[Parameter(Mandatory = $true)]
		[string]$LogPath,

		[Parameter(Mandatory = $true)]
		[string]$StdoutPath,

		[Parameter(Mandatory = $true)]
		[string]$StderrPath
	)

	$previousErrorActionPreference = $ErrorActionPreference
	try {
		$ErrorActionPreference = "Continue"
		& $FilePath @Arguments 1> $StdoutPath 2> $StderrPath
		$nativeExitCode = $LASTEXITCODE
	}
	finally {
		$ErrorActionPreference = $previousErrorActionPreference
	}

	@(
		"[$Stage] $FilePath $($Arguments -join ' ')",
		"--- stdout JSONL ---"
	) | Set-Content -LiteralPath $LogPath -Encoding utf8
	Get-Content -LiteralPath $StdoutPath | Tee-Object -FilePath $LogPath -Append
	"--- stderr ---" | Tee-Object -FilePath $LogPath -Append
	Get-Content -LiteralPath $StderrPath | Tee-Object -FilePath $LogPath -Append
	if ($nativeExitCode -ne 0) {
		throw "Stage '$Stage' failed with exit code $nativeExitCode. See '$LogPath'."
	}

	$records = @()
	foreach ($line in @(Get-Content -LiteralPath $StdoutPath)) {
		if ([string]::IsNullOrWhiteSpace($line)) {
			continue
		}
		try {
			$records += $line | ConvertFrom-Json
		}
		catch {
			throw "Stage '$Stage' wrote non-JSON stdout: '$line'."
		}
	}
	$finalRecords = @($records | Where-Object { $_.type -eq "result" })
	if ($finalRecords.Count -ne 1) {
		throw "Stage '$Stage' must write exactly one final result record; found $($finalRecords.Count)."
	}
}

function Assert-CompiledAsset {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path,

		[Parameter(Mandatory = $true)]
	[ValidateSet("TEMF", "TETF", "TMAT", "TEPC")]
		[string]$Format
	)

	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
		throw "Generated $Format child is missing: '$Path'."
	}
	$bytes = [System.IO.File]::ReadAllBytes($Path)
	if ($bytes.Length -le 4) {
		throw "Generated $Format child is empty or truncated: '$Path'."
	}

	$hasExpectedHeader = switch ($Format) {
		"TETF" { [System.BitConverter]::ToUInt32($bytes, 0) -eq [uint32]0x54455446 }
		default { [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -eq $Format }
	}
	if (-not $hasExpectedHeader) {
		throw "Generated child does not have a valid $Format header: '$Path'."
	}

	# A binary output that still contains the old fixture marker is never valid,
	# even if its first four bytes were edited to resemble a format header.
	$payloadText = [System.Text.Encoding]::ASCII.GetString($bytes)
	if ($payloadText.IndexOf("placeholder", [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
		throw "Generated child contains a placeholder marker: '$Path'."
	}
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$outRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "out"))
$buildRoot = Get-AbsolutePath -Path $BuildDir -BasePath $repoRoot
$resolvedInstallRoot = if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
	Join-Path $buildRoot "Build"
}
else {
	Get-AbsolutePath -Path $InstallRoot -BasePath $repoRoot
}
$fixtureSource = Get-AbsolutePath -Path $FixtureRoot -BasePath $repoRoot
$physicsFixtureSource = Get-AbsolutePath -Path $PhysicsFixtureRoot -BasePath $repoRoot
$workDirectory = Get-AbsolutePath -Path $WorkRoot -BasePath $repoRoot
Assert-PathIsBelow -Parent $outRoot -Child $workDirectory -Description "Disposable baseline work root"

if (-not (Test-Path -LiteralPath $fixtureSource -PathType Container)) {
	throw "Baseline source fixture was not found: '$fixtureSource'."
}
if (-not (Test-Path -LiteralPath $physicsFixtureSource -PathType Container)) {
	throw "Physics export parity fixture was not found: '$physicsFixtureSource'."
}
if (Test-Path -LiteralPath $workDirectory) {
	# The checked boundary above makes this recursive cleanup safe and prevents
	# an empty or misspelled parameter from deleting repository source content.
	Remove-Item -LiteralPath $workDirectory -Recurse -Force
}

$projectRoot = Join-Path $workDirectory "project"
$logRoot = Join-Path $workDirectory "logs"
$evidenceRoot = Join-Path $workDirectory "evidence"
$outputRoot = Join-Path $projectRoot "Builds/Baseline"
New-Item -ItemType Directory -Force -Path $projectRoot, $logRoot | Out-Null

# Copy only authored input directories. Ignored Build/obj folders that a local
# editor may have left beside the fixture are deliberately not copied.
foreach ($fileName in @("project.json", "Assembly-CSharp.csproj")) {
	Copy-Item -LiteralPath (Join-Path $fixtureSource $fileName) -Destination $projectRoot
}
foreach ($directoryName in @("Assets", "BuildProfiles", "ProjectSettings")) {
	Copy-Item -LiteralPath (Join-Path $fixtureSource $directoryName) -Destination $projectRoot -Recurse
}
# Scene, UI, and script sources are authored as separate reviewable fixture
# folders, then placed below Assets in the disposable project because the
# production export pipeline packages that project content root.
foreach ($directoryName in @("Scenes", "Scripts", "UI")) {
	Copy-Item -LiteralPath (Join-Path $fixtureSource $directoryName) -Destination (Join-Path $projectRoot "Assets") -Recurse
}
# The physics fixture stays source-only in the repository. Copy its recipe and
# metadata into the disposable project so the installed compiler is the sole
# producer of the runtime .tepcoll envelope used below.
Copy-Item -LiteralPath (Join-Path $physicsFixtureSource "Assets/Colliders") -Destination (Join-Path $projectRoot "Assets") -Recurse
Copy-Item -LiteralPath (Join-Path $physicsFixtureSource "ProjectSettings/PhysicsSettings.json") -Destination (Join-Path $projectRoot "ProjectSettings/PhysicsSettings.json") -Force

$installedEditor = Join-Path $resolvedInstallRoot "Editor"
$modelCompiler = Resolve-InstalledExecutable -Directory $installedEditor -BaseName "TerraEngineModelPackageCompiler"
$physicsColliderCompiler = Resolve-InstalledExecutable -Directory $installedEditor -BaseName "TerraEnginePhysicsColliderCompiler"
$exportCommand = Resolve-InstalledExecutable -Directory $installedEditor -BaseName "TerraEngine-Export"

$managedReferenceRoot = Join-Path $projectRoot "Build/Assembly-CSharp/References"
New-Item -ItemType Directory -Force -Path $managedReferenceRoot | Out-Null
$managedReferenceInputs = @(
	@{
		Source = Join-Path $installedEditor "Managed/TerraEngine.CSharp.dll"
		Destination = Join-Path $managedReferenceRoot "TerraEngine.CSharp.dll"
	},
	@{
		Source = Join-Path $installedEditor "Managed/Analyzers/TerraEngine.Export.Generator.dll"
		Destination = Join-Path $managedReferenceRoot "TerraEngine.Export.Generator.dll"
	}
)
foreach ($reference in $managedReferenceInputs) {
	if (-not (Test-Path -LiteralPath $reference.Source -PathType Leaf)) {
		throw "Installed managed export input is missing: '$($reference.Source)'."
	}
	Copy-Item -LiteralPath $reference.Source -Destination $reference.Destination
}

$sourceModel = Join-Path $projectRoot "Assets/Models/Baseline.gltf"
$assetRoot = Join-Path $projectRoot "Assets"
$compiledAssetRoot = Join-Path $projectRoot "Build/Assets"
New-Item -ItemType Directory -Force -Path $compiledAssetRoot | Out-Null
$compilerLog = Join-Path $logRoot "model-package-compiler.log"
$compilerArguments = @(
		"--source", $sourceModel,
		"--asset-root", $assetRoot,
		"--build-root", $compiledAssetRoot,
		"--source-relative", "Models/Baseline.gltf",
		"--package-guid", $PackageGuid,
		"--shader-guid", $ShaderGuid
)
Invoke-LoggedNative -Stage "model-package-compile" -FilePath $modelCompiler -Arguments $compilerArguments -LogPath $compilerLog

$importResultPath = "$sourceModel.import.json"
if (-not (Test-Path -LiteralPath $importResultPath -PathType Leaf)) {
	throw "Model compiler did not write its package result sidecar: '$importResultPath'."
}
$importResult = Get-Content -LiteralPath $importResultPath -Raw | ConvertFrom-Json
if ($importResult.status -eq "Failed") {
	throw "Model compiler reported a failed package result. See '$compilerLog'."
}
$missingGeneratedChild = @($importResult.generatedMeshes).Count -eq 0 -or
	@($importResult.generatedMaterials).Count -eq 0 -or
	@($importResult.generatedTextures).Count -eq 0
if ($missingGeneratedChild) {
	throw "Baseline glTF must generate at least one mesh, material, and texture child."
}

foreach ($child in @($importResult.generatedMeshes)) {
	Assert-CompiledAsset -Path (Join-Path $compiledAssetRoot $child.compiledPath) -Format "TEMF"
	if ($child.virtualGeometry.status -ne "Cooked") {
		throw "Generated baseline mesh must contain a current cooked virtual-geometry payload."
	}
}
foreach ($child in @($importResult.generatedMaterials)) {
	Assert-CompiledAsset -Path (Join-Path $compiledAssetRoot $child.compiledPath) -Format "TMAT"
}
foreach ($child in @($importResult.generatedTextures)) {
	Assert-CompiledAsset -Path (Join-Path $compiledAssetRoot $child.compiledPath) -Format "TETF"
}

# Execute a real installed Jolt cook before export. The generated envelope is
# deliberately written only to the disposable compiled-assets directory: the
# source-controlled fixture must remain recipe-only, while the runtime package
# must contain the compatible .tepcoll payload rather than source geometry.
$physicsRecipe = Join-Path $projectRoot "Assets/Colliders/Startup.tecollider"
$physicsCookedCollider = Join-Path $compiledAssetRoot "Colliders/Startup.tepcoll"
$physicsCookLog = Join-Path $logRoot "physics-collider-compiler.log"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $physicsCookedCollider) | Out-Null
Invoke-LoggedNative -Stage "physics-collider-cook" -FilePath $physicsColliderCompiler `
	-Arguments @("--input", $physicsRecipe, "--output", $physicsCookedCollider, "--asset-id", "22222222222222223333333344444444", "--settings", (Join-Path $projectRoot "ProjectSettings/PhysicsSettings.json")) `
	-LogPath $physicsCookLog
Assert-CompiledAsset -Path $physicsCookedCollider -Format "TEPC"
$physicsCookReport = Get-Content -LiteralPath $physicsCookLog -Raw
if ($physicsCookReport -notmatch '"published":true' -or
	$physicsCookReport -notmatch '"providerRevision":"e77f175595e64cb44218cc9d9d56fc365ad0e36a"') {
	throw "Installed physics collider compiler did not publish the required Jolt 5.6.0 cooked envelope. See '$physicsCookLog'."
}

$prepareLog = Join-Path $workDirectory "baseline-prepare.log"
@(
	"projectRoot=$projectRoot",
	"sourceModel=$sourceModel",
	"packageGuid=$PackageGuid",
	"generatedMeshes=$(@($importResult.generatedMeshes).Count)",
	"generatedMaterials=$(@($importResult.generatedMaterials).Count)",
	"generatedTextures=$(@($importResult.generatedTextures).Count)",
	"physicsCookedCollider=$physicsCookedCollider",
	"physicsCookLog=$physicsCookLog",
	"modelCompilerLog=$compilerLog"
) | Set-Content -LiteralPath $prepareLog -Encoding utf8

$profilePath = Join-Path $projectRoot "BuildProfiles/baseline.json"
$exportLog = Join-Path $logRoot "headless-export.log"
$exportStdoutLog = Join-Path $logRoot "headless-export.stdout.jsonl"
$exportStderrLog = Join-Path $logRoot "headless-export.stderr.log"
$exportArguments = @(
		"--project", $projectRoot,
		"--profile", $profilePath,
		"--output", $outputRoot,
		"--evidence", $evidenceRoot,
		"--run-id", $RunId
)
Invoke-LoggedJsonNative -Stage "headless-export" -FilePath $exportCommand -Arguments $exportArguments `
	-LogPath $exportLog -StdoutPath $exportStdoutLog -StderrPath $exportStderrLog

$outputParent = Split-Path -Parent $outputRoot
$transactionPrefix = ".$(Split-Path -Leaf $outputRoot).export"
$orphanTransactionPaths = @(Get-ChildItem -LiteralPath $outputParent -Force |
	Where-Object { $_.Name.StartsWith($transactionPrefix, [System.StringComparison]::Ordinal) })
if ($orphanTransactionPaths.Count -ne 0) {
	throw "Successful export left transaction paths behind: $($orphanTransactionPaths.Name -join ', ')."
}

$requiredEvidence = @(
	"export-result.json",
	"resolved-manifest.json",
	"content-inventory.json",
	"validation-report.json",
	"progress.jsonl",
	"package-validation.txt"
)
foreach ($evidenceName in $requiredEvidence) {
	$evidencePath = Join-Path $evidenceRoot $evidenceName
	if (-not (Test-Path -LiteralPath $evidencePath -PathType Leaf)) {
		throw "Headless export did not produce required evidence '$evidenceName'."
	}
}

$stdoutRecords = @(Get-Content -LiteralPath $exportStdoutLog | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$evidenceRecords = @(Get-Content -LiteralPath (Join-Path $evidenceRoot "progress.jsonl") | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($stdoutRecords.Count -ne $evidenceRecords.Count) {
	throw "CLI stdout and progress evidence contain different record counts."
}
for ($recordIndex = 0; $recordIndex -lt $stdoutRecords.Count; ++$recordIndex) {
	if ($stdoutRecords[$recordIndex] -cne $evidenceRecords[$recordIndex]) {
		throw "CLI stdout and progress evidence differ at record $recordIndex."
	}
}

$exportResult = Get-Content -LiteralPath (Join-Path $evidenceRoot "export-result.json") -Raw | ConvertFrom-Json
if (-not $exportResult.success) {
	throw "Headless export evidence reports failure. See '$exportLog'."
}
$inventory = Get-Content -LiteralPath (Join-Path $evidenceRoot "content-inventory.json") -Raw | ConvertFrom-Json
$placeholderEntries = @($inventory.entries | Where-Object { $_.placeholderDetected })
if ($placeholderEntries.Count -ne 0) {
	throw "Export inventory contains $($placeholderEntries.Count) placeholder-marked entries."
}
foreach ($requiredAssetType in @("mesh", "material", "texture", "scene", "ui", "script")) {
	$matchingEntries = @($inventory.entries | Where-Object { $_.assetType -eq $requiredAssetType })
	if ($matchingEntries.Count -eq 0) {
		throw "Export inventory is missing the required '$requiredAssetType' baseline signal."
	}
}
$managedRuntimePath = Join-Path $outputRoot "Managed/Assembly-CSharp.dll"
if (-not (Test-Path -LiteralPath $managedRuntimePath -PathType Leaf)) {
	throw "Published runtime is missing the NativeAOT gameplay library: '$managedRuntimePath'."
}
$managedRuntimeBytes = [System.IO.File]::ReadAllBytes($managedRuntimePath)
if ($managedRuntimeBytes.Length -eq 0 -or
	[System.Text.Encoding]::ASCII.GetString($managedRuntimeBytes).IndexOf("placeholder", [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
	throw "Published NativeAOT gameplay library is empty or contains a placeholder marker."
}
$manifest = Get-Content -LiteralPath (Join-Path $evidenceRoot "resolved-manifest.json") -Raw | ConvertFrom-Json
$profile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
if ($manifest.startupScene -ne $profile.startupScene) {
	throw "Resolved manifest startup scene '$($manifest.startupScene)' does not match profile '$($profile.startupScene)'."
}
$packageValidation = Get-Content -LiteralPath (Join-Path $evidenceRoot "package-validation.txt") -Raw
if ($packageValidation -notmatch "(?im)^Manifest evidence match:\s*PASS\s*$") {
	throw "Package validation did not prove archived game.json bytes match resolved-manifest.json."
}
if ($packageValidation -notmatch "(?im)^Final:\s*PASS\s*$") {
	throw "Package validation evidence does not contain a final pass signal."
}

# Physics has a separate source-only fixture because its recipe must never ship.
# The export above exercises the installed cook/export path; this inspection
# compares the fixture's durable invariant names to the packaged runtime
# contract without requiring a graphics device in CI.
$physicsFixture = Get-Content -LiteralPath (Join-Path $physicsFixtureSource "project.json") -Raw | ConvertFrom-Json
$expectedPhysicsInvariants = @($physicsFixture.physicsExportInvariants)
if ($expectedPhysicsInvariants.Count -ne 9) {
	throw "Physics parity fixture must declare all nine runtime invariants."
}
if (-not (Test-Path -LiteralPath (Join-Path $physicsFixtureSource "Assets/Colliders/Startup.tecollider") -PathType Leaf)) {
	throw "Physics parity fixture must retain a source-only collider recipe for installed cooking."
}
if ($manifest.physics.provider.identifier -ne "Jolt" -or
	$manifest.physics.provider.version -ne "5.6.0" -or
	$manifest.physics.provider.revision -ne "e77f175595e64cb44218cc9d9d56fc365ad0e36a") {
	throw "Exported physics provider contract must remain Jolt 5.6.0 revision e77f175595e64cb44218cc9d9d56fc365ad0e36a."
}
$physicsInventory = @($inventory.entries | Where-Object { $_.assetType -in @("physics_settings", "physics_material", "cooked_collider") })
if (@($physicsInventory | Where-Object { $_.assetType -eq "physics_settings" }).Count -ne 1) {
	throw "Exported runtime must contain exactly one packaged Physics Settings payload."
}
if (@($physicsInventory | Where-Object { $_.assetType -eq "cooked_collider" -and $_.logicalPath -eq "Colliders/Startup.tepcoll" }).Count -ne 1) {
	throw "Exported runtime must contain the installed Jolt-cooked Startup.tepcoll envelope."
}
if (@($inventory.entries | Where-Object { $_.logicalPath -like "*.tecollider" }).Count -ne 0) {
	throw "Exported runtime retained a source-only collider recipe instead of cooked physics data."
}
$physicsBaselineLog = Join-Path $logRoot "physics-headless-baseline.log"
# The launcher validation switch mounts the published archive, creates no
# renderer window, and returns only after ResourceManager shutdown. It is the
# installed headless launch/shutdown check for this baseline.
$publishedLauncher = Resolve-InstalledExecutable -Directory $outputRoot -BaseName (Split-Path -Leaf $projectRoot)
Invoke-LoggedNative -Stage "physics-headless-launch" -FilePath $publishedLauncher `
	-Arguments @("--validate-export-bootstrap") -LogPath $physicsBaselineLog
$headlessLaunchLog = Get-Content -LiteralPath $physicsBaselineLog -Raw
if ($headlessLaunchLog -notmatch "\[Launcher\] Export bootstrap validation succeeded\.") {
	throw "Published headless physics launcher did not report a successful mount and orderly shutdown."
}
@(
	"physicsFixture=source-only-verified",
	"export=headless",
	"launch=$publishedLauncher --validate-export-bootstrap",
	"invariants=$($expectedPhysicsInvariants -join ',')",
	"provider=Jolt 5.6.0 e77f175595e64cb44218cc9d9d56fc365ad0e36a",
	"shutdown=package-validation-final-pass"
) | Add-Content -LiteralPath $physicsBaselineLog -Encoding utf8

@(
	"result=passed",
	"outputRoot=$outputRoot",
	"evidenceRoot=$evidenceRoot",
	"entryCount=$($inventory.entryCount)",
	"placeholderEntries=0",
	"startupScene=$($manifest.startupScene)",
	"physicsInvariants=$($expectedPhysicsInvariants -join ',')",
	"physicsShutdown=verified"
) | Set-Content -LiteralPath (Join-Path $logRoot "archive-inspection.log") -Encoding utf8

Write-Host "Export runtime baseline passed."
Write-Host "Disposable project: $projectRoot"
Write-Host "Published runtime: $outputRoot"
Write-Host "Evidence: $evidenceRoot"
