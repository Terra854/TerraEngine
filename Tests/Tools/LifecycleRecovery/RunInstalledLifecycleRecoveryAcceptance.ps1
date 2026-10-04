<#
.SYNOPSIS
Validates the installed lifecycle-recovery closure with fresh native/live evidence.
.DESCRIPTION
Configure BUILD_TESTING, build/install, and pass the integrated gates first.
Invoke this installed copy with -InstallRoot <Build> -AllowLiveAcceptance.
The normal editor approval UI remains authoritative. Missing approval, failed
case floors, stale artifacts, or watchdog cleanup cannot produce a passing run.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[string]$WorkRoot='',
	[ValidateRange(1024,65535)][int]$McpPort=43139,
	[switch]$AllowLiveAcceptance
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (!$AllowLiveAcceptance) { throw 'Pass -AllowLiveAcceptance after integrated validation.' }
$install=[IO.Path]::GetFullPath($InstallRoot)
$manifestPath=Join-Path $install 'correctness-artifacts.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or !$manifest.builtAtUtc -or !$manifest.files -or !$manifest.minimumCases) { throw 'Incomplete installed artifact seal.' }
function AssertBelow([string]$Root,[string]$Path) {
	$prefix=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
	if (![IO.Path]::GetFullPath($Path).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes owned root: $Path" }
}
$inventory=@{}
foreach ($entry in $manifest.files) {
	if ([IO.Path]::IsPathRooted($entry.relativePath) -or $inventory.ContainsKey($entry.relativePath)) { throw 'Invalid/duplicate sealed path.' }
	$path=[IO.Path]::GetFullPath((Join-Path $install $entry.relativePath)); AssertBelow $install $path
	if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne [long]$entry.length -or
		(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Missing/stale installed artifact: $path" }
	$inventory[$entry.relativePath]=$path
}
function Installed([string]$Relative) {
	if (!$inventory.ContainsKey($Relative)) { throw "Artifact absent from seal: $Relative" }
	return $inventory[$Relative]
}
if ([IO.Path]::GetFullPath($PSCommandPath) -ne (Installed 'Tests/Tools/LifecycleRecovery/RunInstalledLifecycleRecoveryAcceptance.ps1')) { throw 'Invoke the installed runner.' }
# Export output must be separate from installed runtime inputs. All nested
# runners receive fresh owned paths, so no earlier evidence is removed.
if (!$WorkRoot) { $WorkRoot=Join-Path ([IO.Path]::GetTempPath()) ('TerraEngine-lifecycle-recovery-'+[Guid]::NewGuid().ToString('N')) }
$work=[IO.Path]::GetFullPath($WorkRoot)
$installPrefix=$install.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
$workPrefix=$work.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
if ($work -eq $install -or $work.StartsWith($installPrefix,[StringComparison]::OrdinalIgnoreCase) -or $install.StartsWith($workPrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'WorkRoot and installed runtime must be separate.' }
if (Test-Path -LiteralPath $work) { throw 'WorkRoot must be fresh.' }
New-Item -ItemType Directory -Path $work | Out-Null
$summary=[ordered]@{schemaVersion=1;passed=$false;builtAtUtc=$manifest.builtAtUtc;manifestSha256=(Get-FileHash -LiteralPath $manifestPath).Hash;native=@();live=$null;nativeAot=$null;failure=$null}
$summaryPath=Join-Path $work 'summary.json'
try {
	$engine=Installed 'Tests/Engine/TerraEngine-EngineTests.exe'
	$editor=Installed 'Tests/Editor/TerraEngine-EditorTests.exe'
	$slice=Installed 'Tests/Tools/Correctness/RunNativeCorrectnessSlice.ps1'
	# Named cases contain gated iterations. Counts never substitute iterations
	# for registrations or accept a helper-only replacement for production work.
	$cases=@(@($engine,'Engine.SceneLoadRecovery',14),@($engine,'Engine.NestedTaskSettlement',11),
		@($editor,'Editor.AssetDiscoveryRecovery',8),@($editor,'Editor.AssetImportAdmission',13),
		@($editor,'Editor.ExportArtifactValidation',12),@($editor,'Editor.ExportRetention',10),
		@($engine,'Engine.ArtifactValidation',6),@($engine,'Engine.OwnedImportExecutor',5))
	foreach ($case in $cases) {
		$floor=$manifest.minimumCases.PSObject.Properties[$case[1]]
		if (!$floor -or [int]$floor.Value -lt $case[2]) { throw "Missing/weak case floor: $($case[1])" }
		$logs=Join-Path $work ('native-'+$case[1])
		& $slice -Executable $case[0] -Filter $case[1] -MinimumCases ([int]$floor.Value) -TimeoutSeconds 120 -LogDirectory $logs
		if ($LASTEXITCODE -ne 0) { throw "Native lifecycle slice failed: $($case[1])" }
		$summary.native+=@{filter=$case[1];minimumCases=[int]$floor.Value;logs=$logs}
	}
	$live=Installed 'Tests/Tools/Correctness/RunInstalledCorrectnessAcceptance.ps1'
	$liveWork=Join-Path $work 'live'
	& $live -InstallRoot $install -WorkRoot $liveWork -McpPort $McpPort -AllowLiveAcceptance -LifecycleRecovery
	$liveSummary=Get-Content -LiteralPath (Join-Path $liveWork 'summary.json') -Raw | ConvertFrom-Json
	if (!$liveSummary.passed -or 'dirty-reload-save-as' -notin $liveSummary.live.name -or 'repaired-source-reload' -notin $liveSummary.live.name) { throw 'Live dirty reload/Save As/normal settlement acceptance failed.' }
	$summary.live=Join-Path $liveWork 'summary.json'
	$baseline=Installed 'Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1'
	# The complete authored closure is sealed, including script, physics recipe,
	# scene, project settings and profiles. The runner uses installed tools only.
	$baselineFixture=Split-Path (Installed 'Tests/Fixtures/ExportRuntimeBaseline/game.json')
	$physicsFixture=Split-Path (Split-Path (Installed 'Tests/Fixtures/PhysicsExportRuntime/Scenes/Startup.tescene'))
	$baselineWork=Join-Path $work 'native-aot'
	& $baseline -InstallRoot $install -BuildDir $install -WorkRoot $baselineWork -FixtureRoot $baselineFixture -PhysicsFixtureRoot $physicsFixture -RunId ('lifecycle-'+[Guid]::NewGuid().ToString('N'))
	$baselineProof=Get-Content -LiteralPath (Join-Path $baselineWork 'logs/archive-inspection.log') -Raw
	$validity=Get-Content -LiteralPath (Join-Path $baselineWork 'evidence/native-artifact-validation.json') -Raw | ConvertFrom-Json
	if ($baselineProof -notmatch '(?m)^result=passed\r?$' -or $validity.structure.validity -ne 'valid') { throw 'Real valid-word NativeAOT acceptance failed.' }
	$summary.nativeAot=Join-Path $baselineWork 'evidence'
	$summary.passed=$true
} catch {
	$summary.failure=$_.Exception.Message
	throw
} finally {
	$summary | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
}
Write-Output "Installed lifecycle recovery passed: $summaryPath"
