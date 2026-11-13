#Requires -Version 7.0
<#
Validate CR206 against an installed TerraEngine-Export, without starting the editor.
Prerequisites: an installed executable plus a valid project and build-profile JSON.
Run from the orchestrator after install, for example:
pwsh -File Tests/Tools/HeadlessExport/TestEvidenceContainment.ps1 `
  -ExportExecutable <install>/Editor/TerraEngine-Export.exe `
  -ProjectRoot <fixture-project> -ProfilePath <fixture-profile.json> `
  -WorkRoot <new-private-evidence-directory>
The work root must not exist. It is retained for inspection, never recursively deleted.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$ExportExecutable,
	[Parameter(Mandatory)][string]$ProjectRoot,
	[Parameter(Mandatory)][string]$ProfilePath,
	[Parameter(Mandatory)][string]$WorkRoot
)
$ErrorActionPreference = 'Stop'
$executable = (Resolve-Path -LiteralPath $ExportExecutable).Path
$project = (Resolve-Path -LiteralPath $ProjectRoot).Path
$profile = (Resolve-Path -LiteralPath $ProfilePath).Path
$work = [IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $work) { throw 'WorkRoot must be a new private directory.' }
[void][IO.Directory]::CreateDirectory($work)

function Get-TreeSnapshot([string]$Root) {
	# Include directory names and byte hashes, so newly created evidence also fails
	# when it does not overwrite the sentinel itself.
	$records = @(Get-ChildItem -LiteralPath $Root -Recurse -Force | Sort-Object FullName | ForEach-Object {
		$relative = [IO.Path]::GetRelativePath($Root, $_.FullName)
		if ($_.PSIsContainer) { "directory:$relative" }
		else { "file:${relative}:$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)" }
	})
	return ($records -join "`n")
}

$results = @()
foreach ($name in @('equal', 'evidence-child', 'evidence-parent')) {
	$caseRoot = Join-Path $work $name
	$output = Join-Path $caseRoot 'output'
	$evidence = switch ($name) {
		'equal' { $output }
		'evidence-child' { Join-Path $output 'evidence' }
		'evidence-parent' { $caseRoot }
	}
	[void][IO.Directory]::CreateDirectory($output)
	[IO.File]::WriteAllText((Join-Path $output 'sentinel.bin'), 'private output sentinel')
	$before = Get-TreeSnapshot $caseRoot
	$start = [Diagnostics.ProcessStartInfo]::new($executable)
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	foreach ($argument in @('--project', $project, '--profile', $profile, '--output', $output,
		'--evidence', $evidence, '--run-id', "containment-$name")) { $start.ArgumentList.Add($argument) }
	$process = [Diagnostics.Process]::new()
	$process.StartInfo = $start
	try {
		[void]$process.Start()
		$stdoutTask = $process.StandardOutput.ReadToEndAsync()
		$stderrTask = $process.StandardError.ReadToEndAsync()
		if (-not $process.WaitForExit(30000)) { $process.Kill($true); throw "Argument rejection timed out: $name" }
		$stdout = $stdoutTask.GetAwaiter().GetResult()
		$stderr = $stderrTask.GetAwaiter().GetResult()
		$exitCode = $process.ExitCode
	} finally { $process.Dispose() }
	[IO.File]::WriteAllText((Join-Path $work "$name.stdout.jsonl"), $stdout)
	[IO.File]::WriteAllText((Join-Path $work "$name.stderr.txt"), $stderr)
	$records = @($stdout -split '\r?\n' | Where-Object { $_.Trim().StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
	$record = $records | Select-Object -Last 1
	$unchanged = $before -ceq (Get-TreeSnapshot $caseRoot)
	# Checking the specific overlap message prevents an unrelated bad project or
	# profile from creating a false pass before the containment check is reached.
	$rejected = $null -ne $record -and $exitCode -ne 0 -and
		$record.failureCategory -eq 'invalid_arguments' -and $record.stage -eq 'arguments' -and
		$record.message -eq 'Evidence directory must be outside the transactional output directory.'
	$results += [pscustomobject]@{ case = $name; exitCode = $exitCode; rejectedOverlap = $rejected; treeUnchanged = $unchanged; passed = ($rejected -and $unchanged) }
}
$report = [pscustomobject]@{ cases = $results; passed = (@($results | Where-Object { -not $_.passed }).Count -eq 0) }
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $work 'result.json') -Encoding utf8
$report | ConvertTo-Json -Depth 5
if (-not $report.passed) { throw "Evidence containment failed; inspect $work" }
