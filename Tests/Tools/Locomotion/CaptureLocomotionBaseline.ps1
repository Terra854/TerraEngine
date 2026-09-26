<#[
.SYNOPSIS
    Captures read-only pre-feature build, hardware, test-inventory and character-command evidence.
.DESCRIPTION
    Run from the TerraEngine repository root in PowerShell 7. This script does not configure,
    build, install, or launch an editor. It lists CTest registrations and executes only the
    existing Engine physics character-command regression selected by TERRAENGINE_TEST_FILTER.
.EXAMPLE
    ./Tests/Tools/Locomotion/CaptureLocomotionBaseline.ps1
#>
[CmdletBinding()]
param(
	[string]$OutputDirectory = 'out/evidence/locomotion/060/baseline'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Push-Location $repositoryRoot
try {
	$evidenceRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputDirectory))
	New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
	$buildRoot = Join-Path $repositoryRoot 'out/build/RelWithDebInfo'
	$cachePath = Join-Path $buildRoot 'CMakeCache.txt'
	$engineTests = Join-Path $buildRoot 'TerraEngine-EngineTests.exe'
	if (-not (Test-Path $cachePath) -or -not (Test-Path $engineTests)) {
		throw 'An existing RelWithDebInfo configure and engine test executable are required; this capture never builds them.'
	}

	# Keep raw command output beside the summary so later comparisons can be audited.
	$commit = (& git rev-parse HEAD 2>&1 | Out-String).Trim()
	$cache = Get-Content $cachePath
	$generator = ($cache | Where-Object { $_ -match '^CMAKE_GENERATOR:INTERNAL=' } | Select-Object -First 1) -replace '^CMAKE_GENERATOR:INTERNAL=', ''
	$compiler = ($cache | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:STRING=' } | Select-Object -First 1) -replace '^CMAKE_CXX_COMPILER:STRING=', ''
	$os = Get-CimInstance Win32_OperatingSystem
	$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
	$ramBytes = (Get-CimInstance Win32_PhysicalMemory | Measure-Object -Property Capacity -Sum).Sum
	$gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
		[ordered]@{ name = $_.Name; driverVersion = $_.DriverVersion; adapterRamBytes = $_.AdapterRAM }
	})
	$activePowerScheme = (& powercfg.exe /getactivescheme 2>&1 | Out-String).Trim()
	$editorProcesses = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -match 'TerraEngine-Editor|TerraEngine-Export' } | ForEach-Object {
		[ordered]@{ processId = $_.ProcessId; name = $_.Name; executablePath = $_.ExecutablePath }
	})
	$installedRoot = Join-Path $buildRoot 'Build'
	$installedHashes = @()
	foreach ($relativePath in @('Editor/TerraEngine-Editor.exe', 'Editor/TerraEngine-Export.exe')) {
		$artifactPath = Join-Path $installedRoot $relativePath
		if (Test-Path $artifactPath) {
			$hash = Get-FileHash -Algorithm SHA256 $artifactPath
			$installedHashes += [ordered]@{ path = $artifactPath; sha256 = $hash.Hash }
		}
	}
	$metadata = [ordered]@{
		capturedAtUtc = [DateTime]::UtcNow.ToString('o')
		commit = $commit
		workingTreeStatus = @(& git status --short 2>&1)
		os = [ordered]@{ caption = $os.Caption; version = $os.Version; build = $os.BuildNumber; architecture = $os.OSArchitecture }
		cpu = [ordered]@{ name = $cpu.Name; cores = $cpu.NumberOfCores; logicalProcessors = $cpu.NumberOfLogicalProcessors }
		ramBytes = $ramBytes
		gpus = $gpus
		activePowerScheme = $activePowerScheme
		powershellVersion = $PSVersionTable.PSVersion.ToString()
		cmakeVersion = ((& cmake --version | Select-Object -First 1).ToString())
		ctestVersion = ((& ctest --version | Select-Object -First 1).ToString())
		buildConfiguration = 'RelWithDebInfo'
		cmakeGenerator = $generator
		cmakeCxxCompiler = $compiler
		visualStudioDeveloperEnvironment = [ordered]@{
			vsInstallDirPresent = [bool]$env:VSINSTALLDIR
			vcInstallDirPresent = [bool]$env:VCINSTALLDIR
			vsDevCmdPresent = [bool]$env:VSCMD_VER
			clOnPath = [bool](Get-Command cl.exe -ErrorAction SilentlyContinue)
			ninjaOnPath = [bool](Get-Command ninja.exe -ErrorAction SilentlyContinue)
		}
		installedArtifacts = $installedHashes
		activeEditorOrExportProcesses = $editorProcesses
	}
	$metadata | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $evidenceRoot 'environment.json')

	# CTest -N is an inventory only; do not run any registered suite in this capture.
	$inventory = & ctest --test-dir $buildRoot -C RelWithDebInfo -N 2>&1
	$inventoryExit = $LASTEXITCODE
	$inventory | Set-Content -Encoding utf8 (Join-Path $evidenceRoot 'ctest-inventory.txt')

	# This legacy regression proves deferred native and exported character commands are
	# admitted with default ABI pose fields, and become visible only after physics flush.
	$previousFilter = $env:TERRAENGINE_TEST_FILTER
	try {
		$env:TERRAENGINE_TEST_FILTER = 'Physics.Scheduler.CharacterCommandsAcceptUnusedZeroPoseFields'
		$commandOutput = & $engineTests 2>&1
		$commandExit = $LASTEXITCODE
		$commandOutput | Set-Content -Encoding utf8 (Join-Path $evidenceRoot 'direct-character-command-test.txt')
	}
	finally {
		if ($null -eq $previousFilter) { Remove-Item Env:TERRAENGINE_TEST_FILTER -ErrorAction SilentlyContinue }
		else { $env:TERRAENGINE_TEST_FILTER = $previousFilter }
	}
	[ordered]@{ ctestInventoryExitCode = $inventoryExit; directCharacterCommandExitCode = $commandExit } |
		ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $evidenceRoot 'exit-codes.json')
	if ($inventoryExit -ne 0 -or $commandExit -ne 0) {
		throw 'A baseline command returned nonzero; inspect retained raw output and exit-codes.json.'
	}
	Write-Output "Baseline evidence written to $evidenceRoot"
}
finally {
	Pop-Location
}
