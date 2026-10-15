<#
.SYNOPSIS
Runs one native correctness filter with independent process-tree ownership.
.DESCRIPTION
Build the test and child fixture targets first. This wrapper checks the harness
selected-case summary, so misspelled registrations cannot silently pass.
Example: ./RunNativeCorrectnessSlice.ps1 -Executable ./Build/Tests/Engine/TerraEngine-EngineTests.exe -Filter Audio.WorldCellCapture
Use a fresh log directory; all stdout/stderr and watchdog evidence are retained.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Executable,
	[Parameter(Mandatory = $true)][string]$Filter,
	[ValidateRange(1, 3600)][int]$TimeoutSeconds = 120,
	[ValidateRange(1, 10000)][int]$MinimumCases = 1,
	[string]$LogDirectory = ''
)
$ErrorActionPreference = 'Stop'
# These recovery filters are contracts rather than caller-selected smoke tests.
# Preserve the generic wrapper for existing slices and compound/local filters,
# but never allow an exact recovery slice to undercount its required cases.
$recoveryFloors=@{
	'Engine.SceneMutationFailureSafety'=52
	'Audio.ControlContinuity'=12
	'Audio.PlaybackBinding'=10
	'Audio.SourceSettings'=10
	'Editor.AudioControlPersistence'=4
	'Engine.SceneLoadRecovery'=14
	'Engine.NestedTaskSettlement'=11
	'Editor.AssetDiscoveryRecovery'=8
	'Editor.AssetImportAdmission'=13
	'Editor.ExportArtifactValidation'=12
	'Editor.ExportRetention'=10
	'Engine.ContentArchiveSafety'=30
	'Editor.ContentArchiveSafety'=29
	'Editor.TerrainIndependentCopy'=24
	'Editor.MCP.Transport.HttpAuthority'=4
	'Animation.ProgramPublisher'=9
	# Counts describe registered behavioral cases, not corpus rows/loop iterations.
	'Engine.Resource.Texture.Bounds'=8
	'Engine.Resource.Texture.Rgba'=5
	'Engine.JobGraph.FailureSettlement'=6
	'Engine.UI.HitTesting'=6
	# Feature floors count actual registrations, never repeated corpus rows.
	'Engine.RuntimeMemorySafety.TaskCache'=8
	'Engine.RuntimeMemorySafety.DependencyAdmission'=10
	'Engine.RuntimeMemorySafety.Allocators.FixedPool'=4
	'Engine.RuntimeMemorySafety.Allocators.Alignment'=4
	'Engine.RuntimeMemorySafety.Allocators.Overflow'=6
	'Engine.RuntimeMemorySafety.Allocators'=14
	'Engine.RuntimeMemorySafety.RendererScratch'=10
	'Engine.RuntimeMemorySafety.EmptyFramePolicy'=8
	'Editor.RuntimeMemorySafety.Diagnostics'=4
}
if ($recoveryFloors.ContainsKey($Filter) -and $MinimumCases -lt $recoveryFloors[$Filter]) {
	throw "Recovery filter '$Filter' requires at least $($recoveryFloors[$Filter]) cases."
}
$sliceExecutable = (Resolve-Path -LiteralPath $Executable).Path
if ([string]::IsNullOrEmpty($LogDirectory)) {
	$LogDirectory = Join-Path ([IO.Path]::GetTempPath()) ('TerraEngine-correctness-slice-' + [Guid]::NewGuid().ToString('N'))
}
$sliceLogRoot = [IO.Path]::GetFullPath($LogDirectory)
$sliceOldFilter = [Environment]::GetEnvironmentVariable('TERRAENGINE_TEST_FILTER')
$sliceOldExclude = [Environment]::GetEnvironmentVariable('TERRAENGINE_TEST_EXCLUDE_FILTER')
try {
	$env:TERRAENGINE_TEST_FILTER = $Filter
	Remove-Item Env:TERRAENGINE_TEST_EXCLUDE_FILTER -ErrorAction SilentlyContinue
	& (Join-Path $PSScriptRoot 'RunWithWatchdog.ps1') -Executable $sliceExecutable -TimeoutSeconds $TimeoutSeconds -LogDirectory $sliceLogRoot
	$sliceExit = $LASTEXITCODE
} finally {
	[Environment]::SetEnvironmentVariable('TERRAENGINE_TEST_FILTER', $sliceOldFilter)
	[Environment]::SetEnvironmentVariable('TERRAENGINE_TEST_EXCLUDE_FILTER', $sliceOldExclude)
}
$sliceOutput = [IO.File]::ReadAllText((Join-Path $sliceLogRoot 'stdout.log'))
# A background logger can emit terminal color controls between the count and
# its following whitespace. Normalize only the parsing/display copy; preserve
# raw watchdog bytes so this does not hide diagnostics or weaken the case floor.
$sliceOutput = [regex]::Replace($sliceOutput, '\x1B\[[0-?]*[ -/]*[@-~]', '')
# Worker logging can finish immediately before the harness summary on the same
# line. Match the exact count phrase anywhere, still requiring a numeric token
# boundary and the unchanged case floor; preserve all original log bytes.
# An asynchronous logger can finish a line between the count and its phrase.
# Accept that whitespace without accepting a missing count or reducing a floor.
$sliceSummary = [regex]::Match($sliceOutput, '(?m)(?<![0-9A-Za-z])(\d+)\s+selected tests\b[^\r\n]*')
$sliceOutput -split '\r?\n' | Where-Object { $_ -match '^\[(PASS|FAIL|ERROR)\]' } | Write-Output
if ($sliceExit -ne 0) {
	Write-Error "Native filter failed with exit $sliceExit; evidence: $sliceLogRoot" -ErrorAction Continue
	exit $sliceExit
}
if (-not $sliceSummary.Success -or [int]$sliceSummary.Groups[1].Value -lt $MinimumCases) {
	throw "Native filter '$Filter' selected fewer than $MinimumCases cases; evidence: $sliceLogRoot"
}
Write-Output $sliceSummary.Value
Write-Output ('Native correctness evidence: ' + $sliceLogRoot)
exit 0
