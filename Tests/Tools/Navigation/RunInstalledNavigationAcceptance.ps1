# RunInstalledNavigationAcceptance.ps1 - installed, source-free navigation gate.
# Every matrix is a separate workload record; raw lane output is retained beside it.

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$InstallRoot,
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[string]$WorkDirectory = "",
	[string[]]$Lanes = @("editor", "export-rendered", "export-headless"),
	[string[]]$Matrices = @("semantic", "performance", "idle-overload", "soak", "lifecycle", "usability"),
	[ValidateRange(1, 1440)][int]$DurationMinutes = 30,
	[ValidateRange(0, 86400)][int]$DurationSecondsOverride = 0,
	[ValidateRange(1, 100000)][int]$Cycles = 1,
	[ValidateRange(1, 100)][int]$IdlePairs = 5,
	[ValidateRange(5, 5)][int]$UsabilityInvalidCases = 5,
	[switch]$Qualification,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$install = [IO.Path]::GetFullPath($InstallRoot)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$work = if ([string]::IsNullOrWhiteSpace($WorkDirectory)) { Join-Path $output "work" } else { [IO.Path]::GetFullPath($WorkDirectory) }
$allowedLanes = @("editor", "export-rendered", "export-headless")
$allowedMatrices = @("semantic", "performance", "idle-overload", "soak", "lifecycle", "usability")

function Assert-Contained([string]$Path, [string]$Root, [string]$Label) {
	$fullPath = [IO.Path]::GetFullPath($Path)
	$fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
	if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) { throw "$Label must remain contained under '$Root': '$Path'" }
	return $fullPath
}

function Assert-Outside([string]$Path, [string]$Root, [string]$Label) {
	$fullPath = [IO.Path]::GetFullPath($Path)
	$fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
	if ($fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or $fullPath.Equals($Root, [System.StringComparison]::OrdinalIgnoreCase)) { throw "$Label must be outside the installed tree: '$Path'" }
	return $fullPath
}

foreach ($lane in $Lanes) { if ($lane -notin $allowedLanes) { throw "Unsupported lane '$lane'." } }
foreach ($matrix in $Matrices) { if ($matrix -notin $allowedMatrices) { throw "Unsupported matrix '$matrix'." } }
if (($Matrices -contains "soak") -and (($Lanes -notcontains "export-rendered") -or ($Lanes -notcontains "export-headless"))) { throw "The soak matrix requires rendered and headless export lanes." }
if ($Qualification -and ($Matrices -contains "performance" -or $Matrices -contains "idle-overload") -and $DurationSecondsOverride -eq 0 -and $DurationMinutes -lt 5) { throw "Qualification performance and idle/overload runs require at least 300 seconds." }
if ($Qualification -and ($Matrices -contains "soak") -and $DurationSecondsOverride -eq 0 -and $DurationMinutes -ne 30) { throw "Qualification soak requires exactly 1800 seconds." }
if ($Qualification -and ($Matrices -contains "lifecycle") -and $Cycles -ne 100) { throw "Qualification lifecycle requires exactly 100 cycles." }

New-Item -ItemType Directory -Force -Path $output | Out-Null
$output = Assert-Outside $output $install "OutputDirectory"
$work = Assert-Contained $work $output "WorkDirectory"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$manifest = Join-Path $install "Tests/Fixtures/Navigation/manifest.json"
$runner = Join-Path $install "Tests/Tools/Navigation/TerraEngineNavigationAcceptance.exe"
$comparator = Join-Path $install "Tests/Tools/Navigation/CompareNavigationEvidence.ps1"
$laneExecutables = [ordered]@{
	editor = Join-Path $install "Editor/TerraEngine-Editor.exe"
	"export-rendered" = Join-Path $install "Editor/TerraEngine-Export.exe"
	"export-headless" = Join-Path $install "Editor/TerraEngine-Export.exe"
}
$required = [Collections.Generic.List[string]]::new()
foreach ($path in @($manifest, $runner, $comparator)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $required.Add($path) } }
foreach ($lane in $Lanes) { if (-not (Test-Path -LiteralPath $laneExecutables[$lane] -PathType Leaf)) { $required.Add($laneExecutables[$lane]) } }

$evidence = [ordered]@{
	schemaVersion = [ordered]@{ major = 1; minor = 1 }
	status = if ($required.Count -gt 0) { "unavailable" } elseif ($DryRun) { "dry-run" } else { "pending" }
	installRoot = $install
	outputDirectory = $output
	workDirectory = $work
	lanes = @($Lanes)
	matrices = @($Matrices)
	durationSeconds = if ($DurationSecondsOverride -gt 0) { $DurationSecondsOverride } else { $DurationMinutes * 60 }
	durationSecondsOverride = $DurationSecondsOverride
	cycles = $Cycles
	idlePairs = $IdlePairs
	usabilityInvalidCases = $UsabilityInvalidCases
	qualification = $Qualification -and $DurationSecondsOverride -eq 0
	manifest = $manifest
	missingArtifacts = @($required)
	workloads = @()
	comparison = $null
}

$resultFiles = [Collections.Generic.List[string]]::new()
if ($required.Count -eq 0 -and -not $DryRun) {
	$failed = $false
	# The rendered and headless soak lanes are independent installed processes.
	# Keep their process records here so both lanes can consume the same required
	# 30-minute wall-clock window before their evidence is compared together.
	$pendingSoakRuns = [Collections.Generic.List[object]]::new()
	foreach ($lane in $Lanes) {
		foreach ($matrix in $Matrices) {
			if ($matrix -eq "soak" -and $lane -eq "editor") { continue }
			if ($matrix -eq "performance" -and $lane -ne "export-rendered") { continue }
			# The native runner owns the alternating A/B schedule and emits ten raw
			# records for the required five pairs; keep one schema workload file per lane.
			$iterations = 1
			for ($iteration = 1; $iteration -le $iterations; $iteration++) {
				$workloadName = $matrix
				$laneDirectory = Join-Path (Join-Path $work $lane) $workloadName
				New-Item -ItemType Directory -Force -Path $laneDirectory | Out-Null
				$stdout = Join-Path $laneDirectory "stdout.txt"
				$stderr = Join-Path $laneDirectory "stderr.txt"
				$laneResult = Join-Path $laneDirectory "result.json"
				$duration = if ($matrix -eq "soak" -and $DurationSecondsOverride -eq 0) { 1800 } elseif ($DurationSecondsOverride -gt 0) { $DurationSecondsOverride } elseif ($matrix -in @("semantic", "lifecycle", "usability")) { 1 } else { $DurationMinutes * 60 }
				$agents = if ($Qualification -or $matrix -eq "performance" -or $matrix -eq "idle-overload") { 256 } else { 1 }
				$requests = 100
				$matrixCycles = if ($matrix -eq "lifecycle") { 100 } elseif ($matrix -eq "usability") { $UsabilityInvalidCases } else { 1 }
				$matrixArgument = $matrix
				$qualificationArgument = if ($evidence.qualification) { @("--qualification") } else { @() }
				$arguments = @("--lane", $lane, "--manifest", $manifest, "--output", $laneResult, "--duration-seconds", $duration,
					"--cycles", $matrixCycles, "--agents", $agents, "--requests-per-second", $requests, "--matrices", $matrixArgument,
					"--idle-pairs", $IdlePairs) + $qualificationArgument
				try {
					if ($matrix -eq "soak") {
						# Start-Process provides separate stdout/stderr files without sharing
						# mutable PowerShell state between the two long-running lane processes.
						$process = Start-Process -FilePath $runner -ArgumentList $arguments -RedirectStandardOutput $stdout `
							-RedirectStandardError $stderr -WindowStyle Hidden -PassThru
						$pendingSoakRuns.Add([pscustomobject]@{
							process = $process; lane = $lane; matrix = $matrix; iteration = $iteration
							duration = $duration; workloadName = $workloadName; stdout = $stdout
							stderr = $stderr; laneResult = $laneResult
						})
						continue
					}
					& $runner @arguments > $stdout 2> $stderr
					$exitCode = $LASTEXITCODE
					$hasResult = Test-Path -LiteralPath $laneResult -PathType Leaf
					if ($exitCode -ne 0 -or -not $hasResult) {
						$failed = $true
						if (-not $hasResult) { "Lane '$lane' workload '$workloadName' did not publish result.json." | Set-Content -LiteralPath $stderr -Encoding utf8 }
					}
					if ($hasResult) { $resultFiles.Add($laneResult) }
					$evidence.workloads += [ordered]@{ lane = $lane; matrix = $matrix; iteration = $iteration; durationSeconds = $duration; status = if ($exitCode -eq 0 -and $hasResult) { "passed" } else { "failed" }; exitCode = $exitCode; stdout = $stdout; stderr = $stderr; result = $laneResult }
				} catch {
					$failed = $true
					$_.Exception.Message | Set-Content -LiteralPath $stderr -Encoding utf8
					$evidence.workloads += [ordered]@{ lane = $lane; matrix = $matrix; iteration = $iteration; durationSeconds = $duration; status = "failed"; exitCode = 1; stdout = $stdout; stderr = $stderr; result = $laneResult }
				}
			}
		}
	}
	foreach ($pending in $pendingSoakRuns) {
		try {
			# Waiting after every requested soak lane has started keeps the lanes
			# concurrent while still making comparison wait for complete JSON files.
			$pending.process.WaitForExit()
			$exitCode = $pending.process.ExitCode
			$hasResult = Test-Path -LiteralPath $pending.laneResult -PathType Leaf
			if ($exitCode -ne 0 -or -not $hasResult) {
				$failed = $true
				if (-not $hasResult) { "Lane '$($pending.lane)' workload '$($pending.workloadName)' did not publish result.json." | Set-Content -LiteralPath $pending.stderr -Encoding utf8 }
			}
			if ($hasResult) { $resultFiles.Add($pending.laneResult) }
			$evidence.workloads += [ordered]@{ lane = $pending.lane; matrix = $pending.matrix; iteration = $pending.iteration; durationSeconds = $pending.duration; status = if ($exitCode -eq 0 -and $hasResult) { "passed" } else { "failed" }; exitCode = $exitCode; stdout = $pending.stdout; stderr = $pending.stderr; result = $pending.laneResult }
		} catch {
			$failed = $true
			$_.Exception.Message | Set-Content -LiteralPath $pending.stderr -Encoding utf8
			$evidence.workloads += [ordered]@{ lane = $pending.lane; matrix = $pending.matrix; iteration = $pending.iteration; durationSeconds = $pending.duration; status = "failed"; exitCode = 1; stdout = $pending.stdout; stderr = $pending.stderr; result = $pending.laneResult }
		}
	}
	$comparisonPath = Join-Path $output "navigation-comparison.json"
	$comparisonStdout = Join-Path $work "comparator.stdout.txt"
	$comparisonStderr = Join-Path $work "comparator.stderr.txt"
	try {
		& $comparator -EvidenceFiles @($resultFiles) -OutputPath $comparisonPath > $comparisonStdout 2> $comparisonStderr
		# A successful PowerShell script need not initialize LASTEXITCODE. The
		# automatic success flag is the reliable contract for script invocation.
		$comparisonExit = if ($?) { 0 } else { 1 }
		if ($comparisonExit -ne 0) { $failed = $true }
		$evidence.comparison = [ordered]@{ status = if ($comparisonExit -eq 0) { "passed" } else { "failed" }; exitCode = $comparisonExit; output = $comparisonPath; stdout = $comparisonStdout; stderr = $comparisonStderr }
	} catch {
		$failed = $true
		$_.Exception.Message | Set-Content -LiteralPath $comparisonStderr -Encoding utf8
		$evidence.comparison = [ordered]@{ status = "failed"; exitCode = 1; output = $comparisonPath; stdout = $comparisonStdout; stderr = $comparisonStderr }
	}
	$evidence.status = if ($failed) { "failed" } else { "passed" }
}

$resultPath = Join-Path $output "navigation-acceptance-result.json"
$json = $evidence | ConvertTo-Json -Depth 20
$json | Set-Content -LiteralPath $resultPath -Encoding utf8
Write-Output $json
if ($evidence.status -eq "unavailable" -or $evidence.status -eq "failed") { exit 1 }
