param(
	[string]$BuildDir = "out/build/Debug",
	[string]$Profile = "baseline",
	[string]$WorkRoot = "out/ci/export-runtime-baseline"
)

$ErrorActionPreference = "Stop"
if ($Profile -ne "baseline") {
	throw "Only the standalone 'baseline' build profile is supported by this compatibility helper."
}

# This historical path remains callable for downstream scripts, but all real
# preparation, export, inspection, and failure propagation live in one tool.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../.."))
$runner = Join-Path $repoRoot "Tools/ExportRuntimeBaseline/RunBaseline.ps1"
& $runner -BuildDir $BuildDir -WorkRoot $WorkRoot
