# Run the complete production soak entry point with bounded native protocol
# children. These controls never execute a terrain route or claim measurements.
# Prerequisites: pwsh7 and a compiled Fixtures/TerrainSoakProtocolChild.cpp.
# Optional RealInstallRoot runs one real CPU benchmark, which must stay unavailable.
# Example: pwsh -File TestTerrainSoakFailFast.ps1 -ProtocolChild <child.exe> -EvidenceDirectory <fresh-root>
#requires -Version 7.0
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$ProtocolChild,
	[Parameter(Mandatory=$true)][string]$EvidenceDirectory,
	[string]$SourceRoot = (Join-Path $PSScriptRoot '../../..'),
	[string]$SoakScript,
	[string]$RealInstallRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$ProtocolChild = (Resolve-Path -LiteralPath $ProtocolChild).Path
if ([string]::IsNullOrWhiteSpace($SoakScript)) { $SoakScript = Join-Path $SourceRoot 'Tests/Tools/Terrain/RunTerrainSoak.ps1' }
$SoakScript = (Resolve-Path -LiteralPath $SoakScript).Path
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Fail-fast evidence root must be fresh.' }
[void][IO.Directory]::CreateDirectory($EvidenceDirectory)
$cases = [Collections.Generic.List[object]]::new()
$executions = [Collections.Generic.List[object]]::new()
$hostExecutable = (Get-Process -Id $PID).Path
$oldMode = $env:TERRAENGINE_SOAK_PROTOCOL_MODE
$oldCalls = $env:TERRAENGINE_SOAK_PROTOCOL_CALLS

function Assert-Contract([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Run-Case([string]$Name, [scriptblock]$Body) {
	try { & $Body; $cases.Add([ordered]@{name=$Name;passed=$true}); Write-Output "[PASS] $Name" }
	catch { $cases.Add([ordered]@{name=$Name;passed=$false;reason=$_.Exception.Message}); Write-Output "[FAIL] ${Name}: $($_.Exception.Message)" }
}
function Invoke-Soak([string]$Name, [string]$Mode, [switch]$DryRun, [switch]$MissingExecutable, [string]$ActualInstall) {
	$caseRoot = Join-Path $EvidenceDirectory $Name
	$install = Join-Path $caseRoot 'Installed Bundle With Spaces'
	$output = Join-Path $caseRoot 'Evidence With Spaces'
	[void][IO.Directory]::CreateDirectory($output)
	$tools = Join-Path $install 'Tests/Tools/Terrain'
	$fixtures = Join-Path $install 'Tests/Fixtures/Terrain'
	[void][IO.Directory]::CreateDirectory($tools)
	[void][IO.Directory]::CreateDirectory($fixtures)
	Copy-Item -LiteralPath (Join-Path $SourceRoot 'Tests/Fixtures/Terrain/manifest.json') -Destination (Join-Path $fixtures 'manifest.json')
	Copy-Item -LiteralPath (Join-Path $SourceRoot 'Tests/Tools/Terrain/workloads.json') -Destination (Join-Path $tools 'workloads.json')
	if (-not $MissingExecutable) {
		Copy-Item -LiteralPath $ProtocolChild -Destination (Join-Path $tools 'TerraEngineTerrainFixtureGenerator.exe')
		Copy-Item -LiteralPath $ProtocolChild -Destination (Join-Path $tools 'TerraEngineTerrainBenchmark.exe')
	}
	if (-not [string]::IsNullOrWhiteSpace($ActualInstall)) { $install = (Resolve-Path -LiteralPath $ActualInstall).Path }
	$env:TERRAENGINE_SOAK_PROTOCOL_MODE = $Mode
	$env:TERRAENGINE_SOAK_PROTOCOL_CALLS = Join-Path $caseRoot 'calls.log'
	# A stale previous result is adversarial input, never a fallback result.
	[IO.File]::WriteAllText((Join-Path $output 'terrain-benchmark-result.json'), '{"status":"unavailable","qualification":"stale file must be removed"}')
	$argv = @('-NoProfile','-NonInteractive','-File',$SoakScript,'-InstallRoot',$install,'-OutputDirectory',$output,'-DurationMinutes','30')
	if ($DryRun) { $argv += '-DryRun' }
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $hostExecutable; $start.UseShellExecute=$false; $start.CreateNoWindow=$true
	$start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
	foreach ($argument in $argv) { $start.ArgumentList.Add($argument) }
	$child = [Diagnostics.Process]::new(); $child.StartInfo=$start
	try {
		[void]$child.Start()
		$stdout=$child.StandardOutput.ReadToEndAsync(); $stderr=$child.StandardError.ReadToEndAsync()
		$timedOut = -not $child.WaitForExit(10000)
		if ($timedOut) { $child.Kill($true); [void]$child.WaitForExit(5000) }
		$outText=if($stdout.Wait(5000)){$stdout.Result}else{'<stdout incomplete>'}
		$errText=if($stderr.Wait(5000)){$stderr.Result}else{'<stderr incomplete>'}
		[IO.File]::WriteAllText((Join-Path $caseRoot 'stdout.log'),$outText)
		[IO.File]::WriteAllText((Join-Path $caseRoot 'stderr.log'),$errText)
		$exitCode=if($timedOut){124}else{$child.ExitCode}
		$executions.Add([ordered]@{name=$Name;executable=$hostExecutable;arguments=$argv;exitCode=$exitCode;timedOut=$timedOut;nativeRouteMeasured=$false})
		Assert-Contract (-not $timedOut) 'Unsupported or malformed child evidence must terminate before the external watchdog, without waiting30minutes.'
		$result=Get-Content -Raw -LiteralPath (Join-Path $output 'terrain-soak-result.json')|ConvertFrom-Json
		$calls=@()
		if (Test-Path -LiteralPath $env:TERRAENGINE_SOAK_PROTOCOL_CALLS) { $calls=@(Get-Content -LiteralPath $env:TERRAENGINE_SOAK_PROTOCOL_CALLS) }
		return [pscustomobject]@{result=$result;exitCode=$exitCode;calls=$calls;output=$output}
	} finally { $child.Dispose() }
}

try {
	foreach ($mode in @('unavailable','unavailable-no-qualification')) {
		Run-Case "Soak.UnsupportedStopsImmediately.$mode" {
			$r=Invoke-Soak $mode $mode
			Assert-Contract ($r.exitCode -eq 1 -and $r.result.status -eq 'unavailable' -and $r.result.iterations -eq 1) 'Unsupported producer must report unavailable after exactly one attempted benchmark.'
			Assert-Contract (@($r.calls|Where-Object {$_ -eq 'benchmark'}).Count -eq 1) 'Unsupported result must never start another benchmark.'
			$raw=Get-Content -Raw -LiteralPath $r.result.benchmarkResult|ConvertFrom-Json
			Assert-Contract ($raw.status -eq 'unavailable' -and $null -eq $raw.distanceKilometres -and $r.result.metrics -eq 'unavailable-until-measured-route-execution') 'Raw unsupported route facts must remain honest and must not become measured metrics.'
		}
	}
	foreach ($mode in @('malformed','missing-output','missing-status','invalid-route','exit-failure')) {
		Run-Case "Soak.MalformedOrFailedChildRejects.$mode" {
			$r=Invoke-Soak $mode $mode
			Assert-Contract ($r.exitCode -eq 1 -and $r.result.status -eq 'failed' -and $r.result.iterations -eq 1) 'Invalid producer must fail after its first attempted benchmark.'
			Assert-Contract (@($r.calls|Where-Object {$_ -eq 'benchmark'}).Count -eq 1) 'Invalid evidence must not start another benchmark.'
		}
	}
	Run-Case 'Soak.MissingExecutableIsUnavailableBeforeLaunch' {
		$r=Invoke-Soak 'missing-executable' 'unavailable' -MissingExecutable
		Assert-Contract ($r.exitCode -eq 1 -and $r.result.status -eq 'unavailable' -and $r.result.iterations -eq 0 -and $r.calls.Count -eq 0) 'Missing native prerequisite must remain unavailable with no child work.'
	}
	Run-Case 'Soak.DryRunRemainsUnmeasured' {
		$r=Invoke-Soak 'dry-run' 'unavailable' -DryRun
		Assert-Contract ($r.exitCode -eq 0 -and $r.result.status -eq 'dry-run' -and $r.result.iterations -eq 0 -and $r.calls.Count -eq 0) 'Dry-run is a prerequisite control, never measured terrain evidence.'
	}
	if (-not [string]::IsNullOrWhiteSpace($RealInstallRoot)) {
		Run-Case 'Soak.ActualCpuOnlyBenchmarkRemainsUnavailable' {
			$r=Invoke-Soak 'actual-installed-cpu' 'unavailable' -ActualInstall $RealInstallRoot
			Assert-Contract ($r.exitCode -eq 1 -and $r.result.status -eq 'unavailable' -and $r.result.iterations -eq 1) 'The actual CPU-only benchmark must be unavailable promptly; no real terrain route exists to qualify.'
		}
	}
} finally {
	$env:TERRAENGINE_SOAK_PROTOCOL_MODE=$oldMode
	$env:TERRAENGINE_SOAK_PROTOCOL_CALLS=$oldCalls
}
$failed=@($cases|Where-Object {-not $_.passed}).Count
$report=[ordered]@{qualification='actual production script entrypoint with native protocol fault inputs; no measured terrain/GPU evidence';selectedCases=$cases.Count;failedCases=$failed;cases=$cases.ToArray();executions=$executions.ToArray();sourceSHA256=(Get-FileHash -LiteralPath $SoakScript -Algorithm SHA256).Hash;protocolChildSHA256=(Get-FileHash -LiteralPath $ProtocolChild -Algorithm SHA256).Hash}
$report|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'result.json') -Encoding utf8
Write-Output "$($cases.Count) selected protocol cases, $failed failures"
if ($cases.Count -lt 9 -or $failed -gt 0) { exit 1 }
