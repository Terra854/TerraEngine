param(
	[string]$BuildDir = "build",
	[string]$Profile = "baseline"
)

# Runs the known-good export baseline from a configured TerraEngine build tree.
# Prerequisites:
# - Configure CMake before calling this script.
# - Build the editor/export tooling target for the selected configuration.
# - Replace placeholder TEMF/TETF/TMAT fixtures with compiler-produced binaries
#   when using this script for a runnable manual export.

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$projectRoot = Join-Path $repoRoot "Tests\Fixtures\ExportRuntimeBaseline"
$buildRoot = Join-Path $repoRoot $BuildDir

Write-Host "Export baseline project: $projectRoot"
Write-Host "Build tree: $buildRoot"
Write-Host "Profile: $Profile"
Write-Host "Run the editor export command or CI wrapper against this project/profile."
