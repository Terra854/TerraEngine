[CmdletBinding()]
param(
	[string]$BuildDir = "out/build/RelWithDebInfo",
	[string]$InstallRoot = "",
	[string]$WorkRoot = "out/ci/export-runtime-baseline",
	[string]$FixtureRoot = "Tests/Fixtures/ExportRuntimeBaseline",
	[string]$RunId = "export-runtime-baseline"
)

# Keep the descriptive command name as a compatibility alias while RunBaseline
# remains the canonical command used by documentation, tests, and CI.
$runner = Join-Path $PSScriptRoot "RunBaseline.ps1"
$arguments = @{
	BuildDir = $BuildDir
	WorkRoot = $WorkRoot
	FixtureRoot = $FixtureRoot
	RunId = $RunId
}
if (-not [string]::IsNullOrWhiteSpace($InstallRoot)) {
	$arguments["InstallRoot"] = $InstallRoot
}
& $runner @arguments
