# RunTerrainValidation.ps1 - Installed terrain automation validation contract.
#
# This wrapper is deliberately fail-closed. It validates the complete installed
# artifact set before invoking an optional automation adapter. Dry-run records
# truthful unavailable metrics; live mode requires an adapter which publishes a
# per-operation receipt and never fabricates visible/automated parity.

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$InstallRoot,
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[ValidateSet("all", "create", "inspect", "edit", "rebuild", "validate", "import", "export", "save", "parity")]
	[string]$Operation = "all",
	[string]$AutomationAdapter = "",
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-InstalledFile([string[]]$Candidates) {
	foreach ($candidate in $Candidates) {
		$resolved = Join-Path $install $candidate
		if (Test-Path -LiteralPath $resolved -PathType Leaf) { return [System.IO.Path]::GetFullPath($resolved) }
	}
	return $null
}

function Add-Missing([string]$Path) {
	if ($null -ne $Path -and -not $missing.Contains($Path)) { $missing.Add($Path) }
}

$install = [System.IO.Path]::GetFullPath($InstallRoot)
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($output) | Out-Null
$missing = [System.Collections.Generic.List[string]]::new()
$required = [ordered]@{}

# Request validation belongs to the editor's typed automation boundary. The
# runner supplies fixtures and checks receipts without a separate schema file.
$required.manifest = Resolve-InstalledFile @("Tests/Fixtures/Terrain/manifest.json")
$required.recovery = Resolve-InstalledFile @("Tests/Fixtures/Terrain/SaveRecovery/manifest.json")
$required.workloads = Resolve-InstalledFile @("Tests/Tools/Terrain/workloads.json")
$required.editor = Resolve-InstalledFile @("Editor/TerraEngine-Editor.exe", "Editor/TerraEngine-Editor")
$required.compiler = Resolve-InstalledFile @("Editor/TerraEngineTerrainCompiler.exe", "Editor/TerraEngineTerrainCompiler")
$required.fixtureGenerator = Resolve-InstalledFile @("Tests/Tools/Terrain/TerraEngineTerrainFixtureGenerator.exe", "Tests/Tools/Terrain/TerraEngineTerrainFixtureGenerator")
$required.benchmark = Resolve-InstalledFile @("Tests/Tools/Terrain/TerraEngineTerrainBenchmark.exe", "Tests/Tools/Terrain/TerraEngineTerrainBenchmark")

foreach ($entry in $required.GetEnumerator()) {
	if ($null -eq $entry.Value) { Add-Missing "$install (missing installed $($entry.Key))" }
}

$manifest = $null
if ($null -ne $required.manifest) { $manifest = Get-Content -Raw -LiteralPath $required.manifest | ConvertFrom-Json }
$operations = if ($Operation -eq "all") { @("create", "inspect", "edit", "rebuild", "validate", "import", "export", "save", "parity") } else { @($Operation) }
$resultPath = Join-Path $output "terrain-validation-result.json"
$result = [ordered]@{
	schemaVersion = [ordered]@{ major = 1; minor = 0 }
	status = if ($missing.Count -gt 0) { "unavailable" } elseif ($DryRun) { "dry-run" } else { "unavailable" }
	operation = $Operation
	operations = $operations
	installRoot = $install
	requiredArtifacts = @($required.Values)
	missingArtifacts = @($missing)
	metrics = "unavailable-until-installed-automation-adapter-execution"
	capabilities = [ordered]@{}
	receipts = @()
	failure = $null
}
foreach ($operationName in $operations) { $result.capabilities[$operationName] = "unavailable" }

if (-not $DryRun -and $missing.Count -eq 0) {
	if ([string]::IsNullOrWhiteSpace($AutomationAdapter) -or -not (Test-Path -LiteralPath $AutomationAdapter -PathType Leaf)) {
		$result.failure = "Live terrain validation requires an installed automation adapter; no adapter was supplied."
	} else {
		$receipts = [System.Collections.Generic.List[object]]::new()
		foreach ($operationName in $operations) {
			$receiptPath = Join-Path $output ("terrain-" + $operationName + "-receipt.json")
			& $AutomationAdapter --operation $operationName --fixture $required.manifest --recovery-fixture $required.recovery --output $receiptPath
			if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
				$result.failure = "Automation adapter did not publish a receipt for operation '$operationName'."
				break
			}
			$receipt = Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json
			if ($receipt.status -ne "executed" -or $receipt.operation -ne $operationName) {
				$result.failure = "Receipt for '$operationName' is not an executed receipt for the requested operation."
				break
			}
			$receipts.Add($receipt)
			$result.capabilities[$operationName] = "executed"
		}
		$result.receipts = @($receipts)
		if ($null -eq $result.failure -and $receipts.Count -eq $operations.Count) { $result.status = "executed" }
	}
}

$result | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $resultPath -Encoding utf8
Write-Output (ConvertTo-Json $result -Depth 20)
if ($result.status -eq "unavailable" -or $result.status -eq "failed") { exit 1 }
